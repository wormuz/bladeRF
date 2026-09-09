-- Copyright (c) 2026 Nuand LLC
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

-- Edge cases the plain fifo_reader_tb cannot reach.
--
-- fifo_reader_tb takes its target as a natural and always starts the timestamp
-- counter at zero, so it can only exercise small targets on a fresh counter.
-- The release test is now
--     (timestamp + 2) >= raw_header
-- evaluated a cycle early, with a raw header of zero meaning "transmit now".
-- Both the +2 and the zero sentinel interact with 64-bit wraparound, so this
-- bench takes the counter's start value and the header as std_logic_vector and
-- covers:
--
--   * counter started near all-ones, so timestamp + 2 wraps through zero
--   * a header of all-ones, which the OLD encoding used as the "now" sentinel
--     and the new one must treat as the furthest possible future
--   * a header of zero, which must release immediately and must not depend on
--     arithmetic overflow to do so
--   * reset asserted while a scheduled burst is pending
--
-- Pass criteria are per-case and set by EXPECT_RELEASE: either the gate must
-- open within the run, or it must stay shut for the whole run.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_reader_edge_tb is
    generic (
        -- Value the timestamp counter is loaded with when reset releases.
        TS_START       : std_logic_vector(63 downto 0) := (others => '0');
        -- Raw header timestamp, as written into meta bits 95 downto 32.
        HDR_TIME       : std_logic_vector(63 downto 0) := (others => '0');
        -- true  : the gate must open during the run
        -- false : the gate must stay shut for the whole run
        EXPECT_RELEASE : boolean := true;
        -- Pulse reset again this many cycles after the first release of reset.
        -- 0 disables the second reset.
        RESET_AT       : natural := 0;   -- 0 disables
        CASE_ID        : natural := 0;
        -- Microseconds of run time after reset releases.
        RUN_US         : natural := 40;
        -- Feed a second header once the first has been read, to exercise the
        -- queue transition rather than a single isolated transaction.
        HDR2_ENABLE    : boolean := false;
        HDR2_TIME      : std_logic_vector(63 downto 0) := (others => '0');
        -- Cycles the meta FIFO stays empty between the two headers.
        HDR2_GAP       : natural := 2000;
        TRACE          : boolean := false
    );
end entity;

architecture sim of fifo_reader_edge_tb is

    constant NSTREAMS : natural := 1;

    signal clock      : std_logic := '0';
    signal reset      : std_logic := '1';
    signal enable     : std_logic := '0';
    signal timestamp  : unsigned(63 downto 0) := unsigned(TS_START);

    -- Link epoch stimulus: real firmware signals a new epoch via
    -- link_start_toggle before raising enable, else Stage 3's sticky
    -- protocol-error/abort path (enable with no epoch) halts the datapath.
    -- Reset clears link_active, so re-pulse after the mid-run RESET_AT reset
    -- too.
    signal link_start_toggle : std_logic := '0';

    signal fifo_usedw : std_logic_vector(11 downto 0) := (others => '1');
    signal fifo_read  : std_logic;
    signal fifo_data  : std_logic_vector(63 downto 0) := x"0BAD0BAD0BAD0BAD";

    signal pkt_ctrl   : packet_control_t;
    signal pkt_empty  : std_logic;

    signal m_usedw    : std_logic_vector(2 downto 0) := "001";
    signal m_read     : std_logic;
    signal m_empty    : std_logic := '0';
    signal m_data     : std_logic_vector(127 downto 0);

    signal in_ctrl    : sample_controls_t(0 to NSTREAMS-1) := (others => SAMPLE_CONTROL_ENABLE);
    signal out_smp    : sample_streams_t(0 to NSTREAMS-1);

    signal uf_led     : std_logic;
    signal uf_count   : unsigned(63 downto 0);

    signal reads      : natural := 0;
    signal cycles     : natural := 0;
    signal done       : boolean := false;
    signal hdr_index  : natural range 0 to 1 := 0;
    signal gap_count  : natural := 0;

    function hdr(ts : std_logic_vector(63 downto 0)) return std_logic_vector is
        variable v : std_logic_vector(127 downto 0) := (others => '0');
    begin
        v(95 downto 32) := ts;
        return v;
    end function;

begin

    clock <= not clock after 5 ns when not done else '0';

    -- Present the second header once the first has been consumed, so the
    -- queue-level transition is exercised: pop, load, sentinel decode and the
    -- registered comparison halves all change over on the same cycles.
    --
    -- hdr_index only advances once the FIFO has gone empty and the gap has
    -- elapsed, never on the read itself. A real dcfifo keeps presenting the
    -- word that was read until the next one is pushed; switching m_data on the
    -- read cycle instead lets META_WAIT pick up the SECOND target while still
    -- waiting on the first, which looks like a stall in the DUT but is purely
    -- a modelling error. Measured: with the switch on the read cycle, a first
    -- header of 1000 followed by 3000 released at cycle 3001 rather than 1001,
    -- and followed by all-ones it never released at all.
    m_data <= hdr(HDR_TIME) when hdr_index = 0 else hdr(HDR2_TIME);

    -- With HDR2_ENABLE false this degrades to the single-header behaviour of
    -- fifo_reader_tb: the FIFO goes empty after the first read, so META_LOAD
    -- cannot re-arm and any later read would be a leak.
    meta_drain : process(clock)
    begin
        if rising_edge(clock) then
            if reset = '1' then
                m_empty   <= '0';
                hdr_index <= 0;
            elsif m_read = '1' then
                -- Go empty, but keep presenting the first header: hdr_index
                -- advances only when the gap expires, below.
                m_empty <= '1';
                if hdr_index = 0 and HDR2_ENABLE then
                    gap_count <= HDR2_GAP;
                end if;
            elsif gap_count > 0 then
                gap_count <= gap_count - 1;
                if gap_count = 1 then
                    hdr_index <= 1;
                    m_empty   <= '0';
                end if;
            end if;
        end if;
    end process;

    -- Counter loads TS_START on reset so the wrap cases are reachable.
    ts_proc : process(clock)
    begin
        if rising_edge(clock) then
            if reset = '1' then
                timestamp <= unsigned(TS_START);
            else
                timestamp <= timestamp + 1;
            end if;
        end if;
    end process;

    count_proc : process(clock)
    begin
        if rising_edge(clock) and reset = '0' then
            cycles <= cycles + 1;
            if fifo_read = '1' then
                reads <= reads + 1;
            end if;
            if TRACE then
                if m_read = '1' then
                    report "TRACE c" & integer'image(cycles) &
                           " meta_read hdr_index=" & integer'image(hdr_index)
                        severity note;
                end if;
                if fifo_read = '1' and reads = 0 then
                    report "TRACE c" & integer'image(cycles) & " first fifo_read"
                        severity note;
                end if;
            end if;
        end if;
    end process;

    U_dut : entity work.fifo_reader
        generic map (
            NUM_STREAMS           => NSTREAMS,
            FIFO_READ_THROTTLE    => 0,
            FIFO_USEDW_WIDTH      => fifo_usedw'length,
            FIFO_DATA_WIDTH       => fifo_data'length,
            META_FIFO_USEDW_WIDTH => m_usedw'length,
            META_FIFO_DATA_WIDTH  => m_data'length
        )
        port map (
            clock                 => clock,
            reset                 => reset,
            enable                => enable,
            usb_speed             => '0',
            meta_en               => '1',
            packet_en             => '0',
            eight_bit_mode_en     => '0',
            highly_packed_mode_en => '0',
            timestamp             => timestamp,
            fifo_usedw            => fifo_usedw,
            fifo_read             => fifo_read,
            fifo_empty            => '0',
            fifo_data             => fifo_data,
            fifo_holdoff          => '0',
            packet_control        => pkt_ctrl,
            packet_empty          => pkt_empty,
            packet_ready          => '0',
            meta_fifo_usedw       => m_usedw,
            meta_fifo_read        => m_read,
            meta_fifo_empty       => m_empty,
            meta_fifo_data        => m_data,
            in_sample_controls    => in_ctrl,
            out_samples           => out_smp,
            underflow_led         => uf_led,
            underflow_count       => uf_count,
            underflow_duration    => x"ffff",
            link_start_toggle     => link_start_toggle
        );

    stim : process
    begin
        reset <= '1';
        wait for 20 ns;
        wait until rising_edge(clock);
        reset  <= '0';
        wait until rising_edge(clock);
        link_start_toggle <= not link_start_toggle;
        wait until rising_edge(clock);
        enable <= '1';

        if RESET_AT > 0 then
            for i in 1 to RESET_AT loop
                wait until rising_edge(clock);
            end loop;
            reset <= '1';
            wait until rising_edge(clock);
            wait until rising_edge(clock);
            reset <= '0';
            wait until rising_edge(clock);
            link_start_toggle <= not link_start_toggle;
            wait until rising_edge(clock);
        end if;

        wait for RUN_US * 1 us;

        report "case " & integer'image(CASE_ID) &
               ": ts_start=" & to_hstring(TS_START) &
               " hdr=" & to_hstring(HDR_TIME) &
               " reads=" & integer'image(reads) &
               " cycles=" & integer'image(cycles);

        if EXPECT_RELEASE then
            assert reads > 0
                report "case " & integer'image(CASE_ID) & ": gate never opened, expected release"
                severity failure;
            report "case " & integer'image(CASE_ID) & ": released as expected";
        else
            assert reads = 0
                report "case " & integer'image(CASE_ID) & ": gate opened but must stay shut (" &
                       integer'image(reads) & " reads)"
                severity failure;
            report "case " & integer'image(CASE_ID) & ": stayed shut as expected";
        end if;

        done <= true;
        wait;
    end process;

end architecture;
