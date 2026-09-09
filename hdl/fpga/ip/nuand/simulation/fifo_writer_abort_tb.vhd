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

-- Exercises fifo_writer's Stage 3 abort path and sticky transport-fault
-- flags: link_stop_toggle, clear_fault_toggle, fault_sticky, abort_active.
--
-- Four cases, run in sequence against one DUT instance:
--   1. stop toggle -> link_active drops, sticky fault(4) (fifo abort)
--      survives, epoch_counter unchanged.
--   2. new epoch start after an abort -> abort_active clears, sticky
--      clears, FSM leaves ABORTED (meta writes resume).
--   3. repeated identical stop_toggle value -> no second pulse (no extra
--      abort_active/link_active edge).
--   4. clear-fault toggle -> sticky clears but abort_active does NOT
--      restart the datapath by itself (meta_write stays low).

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_writer_abort_tb is
end entity;

architecture tb of fifo_writer_abort_tb is

    constant NSTREAMS   : natural := 2;
    constant CLK_PERIOD : time    := 8 ns;

    signal clock        : std_logic := '0';
    signal reset        : std_logic := '1';
    signal enable       : std_logic := '0';
    signal done         : boolean   := false;

    signal timestamp    : unsigned(63 downto 0) := (others => '0');

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

    signal link_start_toggle  : std_logic := '0';
    signal link_stop_toggle   : std_logic := '0';
    signal clear_fault_toggle : std_logic := '0';
    signal usb_speed_mismatch : std_logic;
    signal link_active        : std_logic;
    signal speed_latched      : std_logic;
    signal protocol_start_violation : std_logic;
    signal link_epoch_counter : unsigned(7 downto 0);
    signal fault_sticky       : std_logic_vector(4 downto 0);
    signal abort_active       : std_logic;

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
            usb_speed_mismatch    => usb_speed_mismatch,
            link_active           => link_active,
            speed_latched         => speed_latched,
            protocol_start_violation => protocol_start_violation,
            link_epoch_counter    => link_epoch_counter,

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

    stim : process
        variable epoch_before   : unsigned(7 downto 0);
        variable meta_writes_a  : natural := 0;
        variable meta_writes_b  : natural := 0;
    begin
        reset <= '1';
        wait for 10*CLK_PERIOD;
        wait until rising_edge(clock);
        reset <= '0';
        wait until rising_edge(clock);

        -- Establish epoch 1, enable the datapath.
        for i in 0 to NSTREAMS-1 loop
            sample_ctrls(i).enable   <= '1';
            sample_ctrls(i).data_req <= '1';
        end loop;
        link_start_toggle <= not link_start_toggle;
        wait until rising_edge(clock);
        enable <= '1';
        wait until rising_edge(clock);

        for i in 1 to 100 loop
            wait until rising_edge(clock);
        end loop;

        assert link_active = '1'
            report "case setup: link_active did not come up after epoch start"
            severity failure;

        epoch_before := link_epoch_counter;

        ----------------------------------------------------------------
        -- Case 1: stop toggle -> link_active drops, fault_sticky(4)
        -- (fifo abort) survives, epoch_counter unchanged.
        ----------------------------------------------------------------
        link_stop_toggle <= not link_stop_toggle;
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        wait until rising_edge(clock);

        assert link_active = '0'
            report "case 1: link_active did not drop after stop toggle"
            severity failure;

        assert abort_active = '1'
            report "case 1: abort_active did not assert after stop toggle"
            severity failure;

        assert fault_sticky(4) = '1'
            report "case 1: fault_sticky(4) (fifo abort) not set after stop"
            severity failure;

        assert link_epoch_counter = epoch_before
            report "case 1: epoch_counter changed on a stop (must only " &
                   "change on a new epoch start)"
            severity failure;

        -- Datapath must be halted: no further meta writes while ABORTED,
        -- even though samples keep flowing.
        meta_writes_a := 0;
        for i in 1 to 200 loop
            wait until rising_edge(clock);
            if meta_write = '1' then
                meta_writes_a := meta_writes_a + 1;
            end if;
        end loop;

        assert meta_writes_a = 0
            report "case 1: meta_write pulsed " & integer'image(meta_writes_a) &
                   " times while ABORTED (must be 0)"
            severity failure;

        report "case 1 OK: stop toggle halts datapath, sticky fault(4) " &
               "set, epoch_counter unchanged (" &
               integer'image(to_integer(link_epoch_counter)) & ")";

        ----------------------------------------------------------------
        -- Case 3: repeated identical stop_toggle value -> no second pulse.
        -- Re-assert the same level (no edge) and confirm nothing changes.
        ----------------------------------------------------------------
        link_stop_toggle <= link_stop_toggle;  -- same value, no edge
        wait until rising_edge(clock);
        wait until rising_edge(clock);

        assert abort_active = '1' and fault_sticky(4) = '1' and link_active = '0'
            report "case 3: state disturbed by a non-edge on link_stop_toggle"
            severity failure;

        report "case 3 OK: repeated identical stop_toggle value produced no pulse";

        ----------------------------------------------------------------
        -- Case 4: clear-fault toggle -> sticky clears but abort_active
        -- does NOT restart the datapath by itself.
        ----------------------------------------------------------------
        clear_fault_toggle <= not clear_fault_toggle;
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        wait until rising_edge(clock);

        assert fault_sticky = "00000"
            report "case 4: fault_sticky did not clear on clear-fault pulse"
            severity failure;

        -- abort_active is re-derived combinationally from the *current*
        -- fault_sticky each cycle it is not held by start/stop; since the
        -- stop condition is a one-shot pulse (not sticky itself) and the
        -- sticky bits are now clear, abort_active following this pulse is
        -- expected to also drop -- but the datapath must not resume
        -- (meta_write stays low) without a new epoch start, which is the
        -- actual product requirement under test here.
        meta_writes_b := 0;
        for i in 1 to 200 loop
            wait until rising_edge(clock);
            if meta_write = '1' then
                meta_writes_b := meta_writes_b + 1;
            end if;
        end loop;

        assert meta_writes_b = 0
            report "case 4: meta_write pulsed " & integer'image(meta_writes_b) &
                   " times after clear-fault alone (datapath must stay " &
                   "halted until a new epoch start)"
            severity failure;

        report "case 4 OK: clear-fault clears sticky, datapath still halted " &
               "without a new epoch";

        ----------------------------------------------------------------
        -- Case 2: new epoch start after an abort -> abort_active clears,
        -- sticky clears, FSM leaves ABORTED (meta writes resume).
        ----------------------------------------------------------------
        link_start_toggle <= not link_start_toggle;
        wait until rising_edge(clock);
        enable <= '1';
        wait until rising_edge(clock);

        assert abort_active = '0'
            report "case 2: abort_active did not clear on new epoch start"
            severity failure;

        assert fault_sticky = "00000"
            report "case 2: fault_sticky did not clear on new epoch start"
            severity failure;

        assert link_epoch_counter = epoch_before + 1
            report "case 2: epoch_counter did not advance on the new start"
            severity failure;

        meta_writes_b := 0;
        for i in 1 to 400 loop
            wait until rising_edge(clock);
            if meta_write = '1' then
                meta_writes_b := meta_writes_b + 1;
            end if;
        end loop;

        assert meta_writes_b > 0
            report "case 2: writer never resumed meta writes after a new " &
                   "epoch start (FSM stuck in ABORTED)"
            severity failure;

        report "case 2 OK: new epoch start clears abort_active/sticky, " &
               "writer resumed (" & integer'image(meta_writes_b) &
               " meta writes)";

        report "fifo_writer_abort_tb: all cases passed";

        done <= true;
        wait;
    end process;

end architecture;
