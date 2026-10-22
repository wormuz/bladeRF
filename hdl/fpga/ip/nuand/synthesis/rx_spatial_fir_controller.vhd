-- sys_clock -> rx_clock control path of rx_spatial_fir.
-- Design: doc/development/rx-spatial-mimo-fir-design.md, section 2.3.
--
-- One work.handshake carries {rx_sfir_data, rx_sfir_ctrl} (64 bits) with the
-- continuous req/ack cycle and a capture-on-ack register (control_rx), same
-- shape as rx_epoch_controller. Toggle edges are decoded against the
-- previous captured word and turned into single-cycle rx_clock pulses for
-- the datapath, which owns the shadow/active banks.
--
-- rx_sfir_ctrl:
--   4:0    write index 0..19
--   5      write toggle: on change, shadow[idx] <= data
--   6      commit toggle: on change, active <= shadow, tag <= ctrl[23:16]
--   7      force_identity level
--   23:16  commit tag
--
-- If one capture carries both a write and a commit edge, the write is issued
-- first and the commit two cycles later, so the commit takes the new word
-- and its identity flag. The echo bits change only after the datapath has
-- taken the action, so a
-- status snapshot with the new echo also carries the new bank state.
--
-- After rx_reset the first capture only adopts the surviving toggle levels
-- (NIOS and its PIO are not reset with the fabric), so a toggle left by an
-- earlier session never fires. force_identity reads '0' until then.
--
-- Copyright (c) 2026 bladeRF project contributors
--
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
--
-- The above copyright notice and this permission notice shall be included in
-- all copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
-- THE SOFTWARE.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity rx_spatial_fir_controller is
    port (
        source_clock    : in  std_logic;
        source_reset    : in  std_logic;
        source_data     : in  std_logic_vector(31 downto 0);
        source_ctrl     : in  std_logic_vector(31 downto 0);

        rx_clock        : in  std_logic;
        rx_reset        : in  std_logic;

        -- To rx_spatial_fir (rx_clock pulses / level).
        coef_wr_en      : out std_logic;
        coef_wr_idx     : out unsigned(4 downto 0);
        coef_wr_data    : out std_logic_vector(31 downto 0);
        coef_commit     : out std_logic;
        coef_commit_tag : out unsigned(7 downto 0);
        force_identity  : out std_logic;

        -- Status bits [7:0] of rx_sfir_status: [4:0] last written index,
        -- [5] write echo, [6] commit echo, [7] force_identity as applied.
        status_ctrl     : out std_logic_vector(7 downto 0)
    );
end entity;

architecture rtl of rx_spatial_fir_controller is
    signal request_rx      : std_logic := '0';
    signal acknowledge_rx  : std_logic;
    signal control_wire_rx : std_logic_vector(63 downto 0);
    signal source_word     : std_logic_vector(63 downto 0);
    -- [63:32] data, [31:0] ctrl
    signal control_rx      : std_logic_vector(63 downto 0) := (others => '0');

    signal initialized     : std_logic := '0';
    signal init_pending    : std_logic := '0';
    signal prev_wr_t       : std_logic := '0';
    signal prev_cm_t       : std_logic := '0';

    signal wr_en_q         : std_logic := '0';
    signal wr_idx_q        : unsigned(4 downto 0) := (others => '0');
    signal wr_data_q       : std_logic_vector(31 downto 0) := (others => '0');
    signal commit_req      : std_logic := '0';
    signal commit_p        : std_logic := '0';
    signal commit_q        : std_logic := '0';
    signal commit_tag_q    : unsigned(7 downto 0) := (others => '0');
    signal force_q         : std_logic := '0';

    signal wr_echo_req     : std_logic := '0';
    signal wr_echo         : std_logic := '0';
    signal cm_echo_req     : std_logic := '0';
    signal cm_echo         : std_logic := '0';
    signal last_idx        : unsigned(4 downto 0) := (others => '0');
begin
    source_word <= source_data & source_ctrl;

    transfer : entity work.handshake
        generic map (DATA_WIDTH => 64)
        port map (
            source_reset => source_reset,
            source_clock => source_clock,
            source_data  => source_word,
            dest_reset   => rx_reset,
            dest_clock   => rx_clock,
            dest_data    => control_wire_rx,
            dest_req     => request_rx,
            dest_ack     => acknowledge_rx
        );

    drive_request : process(rx_clock, rx_reset)
    begin
        if rx_reset = '1' then
            request_rx <= '0';
        elsif rising_edge(rx_clock) then
            if acknowledge_rx = '0' then
                request_rx <= '1';
            else
                request_rx <= '0';
            end if;
        end if;
    end process;

    capture_control : process(rx_clock, rx_reset)
    begin
        if rx_reset = '1' then
            control_rx <= (others => '0');
        elsif rising_edge(rx_clock) then
            if acknowledge_rx = '1' then
                control_rx <= control_wire_rx;
            end if;
        end if;
    end process;

    decode : process(rx_clock, rx_reset)
    begin
        if rx_reset = '1' then
            initialized  <= '0';
            init_pending <= '0';
            prev_wr_t    <= '0';
            prev_cm_t    <= '0';
            wr_en_q      <= '0';
            wr_idx_q     <= (others => '0');
            wr_data_q    <= (others => '0');
            commit_req   <= '0';
            commit_p     <= '0';
            commit_q     <= '0';
            commit_tag_q <= (others => '0');
            force_q      <= '0';
            wr_echo_req  <= '0';
            wr_echo      <= '0';
            cm_echo_req  <= '0';
            cm_echo      <= '0';
            last_idx     <= (others => '0');
        elsif rising_edge(rx_clock) then
            -- rx_spatial_fir's shadow -> S1 hold waiver needs these apart:
            -- one capture issues the write first and the commit two cycles
            -- later, and captures are a full handshake apart.
            assert not (wr_en_q = '1' and commit_q = '1')
                report "rx_spatial_fir_controller: write and commit together"
                severity failure;
            wr_en_q    <= '0';
            commit_req <= '0';
            -- Two edges after the write: rx_spatial_fir compares each
            -- shadow word with its identity one edge after the write lands.
            commit_p   <= commit_req;
            commit_q   <= commit_p;

            -- Echo follows the action: write lands in the shadow on the edge
            -- after wr_en_q, commit in the bank on the edge after commit_q.
            wr_echo     <= wr_echo_req;
            if commit_q = '1' then
                cm_echo <= cm_echo_req;
            end if;

            if init_pending = '1' then
                -- Baseline from the capture register, not the raw wire.
                prev_wr_t    <= control_rx(5);
                prev_cm_t    <= control_rx(6);
                wr_echo_req  <= control_rx(5);
                cm_echo_req  <= control_rx(6);
                cm_echo      <= control_rx(6);
                last_idx     <= unsigned(control_rx(4 downto 0));
                initialized  <= '1';
                init_pending <= '0';
            elsif acknowledge_rx = '1' and initialized = '0' then
                init_pending <= '1';
            elsif initialized = '1' then
                force_q <= control_rx(7);
                if control_rx(5) /= prev_wr_t then
                    wr_en_q     <= '1';
                    wr_idx_q    <= unsigned(control_rx(4 downto 0));
                    wr_data_q   <= control_rx(63 downto 32);
                    last_idx    <= unsigned(control_rx(4 downto 0));
                    wr_echo_req <= control_rx(5);
                end if;
                if control_rx(6) /= prev_cm_t then
                    commit_req   <= '1';
                    commit_tag_q <= unsigned(control_rx(23 downto 16));
                    cm_echo_req  <= control_rx(6);
                end if;
                prev_wr_t <= control_rx(5);
                prev_cm_t <= control_rx(6);
            end if;
        end if;
    end process;

    coef_wr_en      <= wr_en_q;
    coef_wr_idx     <= wr_idx_q;
    coef_wr_data    <= wr_data_q;
    coef_commit     <= commit_q;
    coef_commit_tag <= commit_tag_q;
    force_identity  <= force_q;

    status_ctrl <= force_q & cm_echo & wr_echo & std_logic_vector(last_idx);
end architecture;
