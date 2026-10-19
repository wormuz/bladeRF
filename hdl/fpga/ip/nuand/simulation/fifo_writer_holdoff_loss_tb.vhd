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

-- Checks the RX loss accounting of fifo_writer's HOLDOFF state.
--
-- A valid sample that arrives while the writer sits in HOLDOFF waiting for
-- room for a whole DMA buffer (fifo_enough false, FIFO not yet full) is
-- dropped. detect_overflows must count it, unless it is an intentional epoch
-- fence (rx_epoch_discard_active = '1'). Packet boundaries with room
-- available pass through HOLDOFF for one cycle and must not count.
--
-- Phases:
--   A  free FIFO, several packets: overflow_count stays 0, writes continue
--   B  free space < dma_buf_size, fifo_full = '0': overflow_count >= 1 and
--      no FIFO writes
--   C  FIFO freed: writes resume
--   D  same stall with discard active from before the stall: counter holds;
--      discard released while still stalled: counter grows (control)
--   E  FIFO freed again: writes resume, counter no longer grows

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_writer_holdoff_loss_tb is
end entity;

architecture tb of fifo_writer_holdoff_loss_tb is

    constant NSTREAMS   : natural := 2;
    constant CLK_PERIOD : time    := 8 ns;
    -- usedw 3000 of 4096 leaves 1096 < GPIF_BUF_SIZE_SS (2048)
    constant USEDW_TIGHT : natural := 3000;

    signal clock        : std_logic := '0';
    signal reset        : std_logic := '1';
    signal enable       : std_logic := '0';
    signal done         : boolean   := false;

    signal timestamp    : unsigned(63 downto 0) := (others => '0');
    signal epoch_id     : unsigned(7 downto 0) := x"2A";

    signal sample_ctrls : sample_controls_t(0 to NSTREAMS-1) :=
                              (others => SAMPLE_CONTROL_DISABLE);
    signal samples      : sample_streams_t(0 to NSTREAMS-1) :=
                              (others => ZERO_SAMPLE);

    signal fifo_usedw   : std_logic_vector(11 downto 0) := (others => '0');
    signal fifo_clear   : std_logic;
    signal fifo_write   : std_logic;
    signal fifo_data    : std_logic_vector(63 downto 0);

    signal meta_usedw   : std_logic_vector(4 downto 0) := (others => '0');
    signal meta_data    : std_logic_vector(127 downto 0);
    signal meta_write   : std_logic;

    signal packet_ready : std_logic;
    signal ovf_led      : std_logic;
    signal ovf_count    : unsigned(63 downto 0);

    signal discard_active     : std_logic := '0';
    signal link_start_toggle  : std_logic := '0';
    signal link_stop_toggle   : std_logic := '0';
    signal clear_fault_toggle : std_logic := '0';
    signal fault_sticky       : std_logic_vector(5 downto 0);
    signal abort_active       : std_logic;

    signal writes_total : natural := 0;
    signal metas_total  : natural := 0;

begin

    clock <= not clock after CLK_PERIOD/2 when not done else '0';

    U_dut : entity work.fifo_writer
        generic map (
            NUM_STREAMS           => NSTREAMS,
            FIFO_USEDW_WIDTH      => fifo_usedw'length,
            FIFO_DATA_WIDTH       => fifo_data'length,
            META_FIFO_USEDW_WIDTH => meta_usedw'length,
            META_FIFO_DATA_WIDTH  => meta_data'length
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
            mini_exp              => (others => '0'),
            rx_epoch_meta_enable  => '1',
            rx_epoch_id           => epoch_id,
            rx_epoch_discard_active => discard_active,

            in_sample_controls    => sample_ctrls,
            in_samples            => samples,

            fifo_usedw            => fifo_usedw,
            fifo_clear            => fifo_clear,
            fifo_write            => fifo_write,
            fifo_full             => '0',
            fifo_data             => fifo_data,

            packet_control        => PACKET_CONTROL_DEFAULT,
            packet_ready          => packet_ready,

            meta_fifo_full        => '0',
            meta_fifo_usedw       => meta_usedw,
            meta_fifo_data        => meta_data,
            meta_fifo_write       => meta_write,

            overflow_led          => ovf_led,
            overflow_count        => ovf_count,
            overflow_duration     => to_unsigned(0, 16),

            link_start_toggle     => link_start_toggle,
            link_stop_toggle      => link_stop_toggle,
            clear_fault_toggle    => clear_fault_toggle,
            fault_sticky          => fault_sticky,
            abort_active          => abort_active
        );

    ts_proc : process(clock)
    begin
        if rising_edge(clock) and reset = '0' then
            timestamp <= timestamp + 1;
        end if;
    end process;

    feed_proc : process(clock)
    begin
        if rising_edge(clock) then
            for i in 0 to NSTREAMS-1 loop
                samples(i).data_i <= to_signed(i*256 + 1, 16);
                samples(i).data_q <= to_signed(i*256 + 2, 16);
                samples(i).data_v <= sample_ctrls(i).enable;
            end loop;
        end if;
    end process;

    obs_proc : process(clock)
    begin
        if rising_edge(clock) and reset = '0' then
            if fifo_write = '1' then
                writes_total <= writes_total + 1;
            end if;
            if meta_write = '1' then
                metas_total <= metas_total + 1;
            end if;
        end if;
    end process;

    stim : process
        variable w0, m0 : natural;
        variable c0     : unsigned(63 downto 0);

        procedure ticks(n : natural) is
        begin
            for i in 1 to n loop
                wait until rising_edge(clock);
            end loop;
        end procedure;

        procedure stall(on_off : boolean) is
        begin
            if on_off then
                fifo_usedw <= std_logic_vector(to_unsigned(USEDW_TIGHT, 12));
            else
                fifo_usedw <= (others => '0');
            end if;
        end procedure;
    begin
        reset <= '1';
        wait for 10*CLK_PERIOD;
        wait until rising_edge(clock);
        reset <= '0';
        ticks(12);

        for i in 0 to NSTREAMS-1 loop
            sample_ctrls(i).enable   <= '1';
            sample_ctrls(i).data_req <= '1';
        end loop;
        enable <= '1';
        link_start_toggle <= not link_start_toggle;

        -- Phase A: stable stream, several packets, free FIFO
        ticks(9000);
        assert writes_total > 0 and metas_total >= 3
            report "phase A: stream not running, writes=" &
                   integer'image(writes_total) & " metas=" &
                   integer'image(metas_total)
            severity failure;
        assert ovf_count = 0
            report "phase A: false overflow count " &
                   integer'image(to_integer(ovf_count))
            severity failure;
        report "phase A ok: writes=" & integer'image(writes_total) &
               " metas=" & integer'image(metas_total) & " ovf=0";

        -- Phase B: room < dma_buf_size, FIFO not full
        stall(true);
        ticks(6000);
        assert ovf_count >= 1
            report "phase B: HOLDOFF loss not counted" severity failure;
        w0 := writes_total;
        ticks(400);
        assert writes_total = w0
            report "phase B: writer still writing into a FIFO without room"
            severity failure;
        assert abort_active = '0' and fault_sticky = "000000"
            report "phase B: unexpected abort/fault" severity failure;
        report "phase B ok: ovf=" & integer'image(to_integer(ovf_count)) &
               " writes frozen at " & integer'image(w0);

        -- Phase C: FIFO freed
        stall(false);
        ticks(3000);
        assert writes_total > w0
            report "phase C: writes did not resume" severity failure;
        ticks(10);

        -- Phase D: discard active before the stall begins
        c0 := ovf_count;
        discard_active <= '1';
        ticks(4);
        stall(true);
        ticks(6000);
        w0 := writes_total;
        ticks(400);
        assert writes_total = w0
            report "phase D: writer still writing into a FIFO without room"
            severity failure;
        assert ovf_count = c0
            report "phase D: loss counted during epoch discard: " &
                   integer'image(to_integer(c0)) & " -> " &
                   integer'image(to_integer(ovf_count))
            severity failure;
        -- control: same stall, discard released, must now count
        discard_active <= '0';
        ticks(50);
        assert ovf_count > c0
            report "phase D: control failed, stall not counted after discard released"
            severity failure;
        report "phase D ok: counter held at " & integer'image(to_integer(c0)) &
               " under discard, " & integer'image(to_integer(ovf_count)) &
               " after release";

        -- Phase E: FIFO freed, writes resume, counter settles
        stall(false);
        ticks(3000);
        assert writes_total > w0
            report "phase E: writes did not resume" severity failure;
        c0 := ovf_count;
        m0 := metas_total;
        ticks(9000);
        assert metas_total > m0 + 2
            report "phase E: stream not continuing" severity failure;
        assert ovf_count = c0
            report "phase E: counter grew with free FIFO" severity failure;

        report "fifo_writer_holdoff_loss_tb: PASS" severity note;
        done <= true;
        wait;
    end process;

end architecture;
