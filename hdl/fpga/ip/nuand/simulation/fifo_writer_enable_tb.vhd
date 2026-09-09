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

-- Exercises a live change of the per-channel enable while samples are flowing.
--
-- The MIMO patch in fifo_writer's meta FSM reads the enable bits to decide
-- whether to force the state back to IDLE. That read was moved from the
-- unregistered port to the local registered copy in_sample_controls_r, which
-- was the design-wide worst setup path (adc_enable through top-level glue into
-- meta_current.state). The registered copy lags by exactly one clock, so this
-- bench toggles the enables at a range of offsets relative to the metadata
-- window and checks the writer keeps making forward progress and keeps its
-- metadata paired with the sample bursts it wrote.
--
-- What it asserts, per enable-toggle offset:
--   * the writer never stops producing (no FSM lock-up after a live toggle)
--   * every meta write is accompanied by at least one sample write, so no
--     metadata header is emitted for a burst that never happened
--   * disabling both channels quiesces the writer instead of free-running

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_writer_enable_tb is
    generic (
        -- Clock offset, in cycles after RX starts, at which the enables are
        -- toggled. Swept by the runner so the toggle lands in different FSM
        -- states including the metadata window.
        TOGGLE_AT : natural := 300
    );
end entity;

architecture tb of fifo_writer_enable_tb is

    constant NSTREAMS   : natural := 2;   -- MIMO: the patch only fires for 2
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
    -- fifo_writer asserts fifo_data'length >= NUM_STREAMS*2*16, so two MIMO
    -- streams of 16-bit I and Q need 64 bits.
    signal fifo_data    : std_logic_vector(63 downto 0);

    signal meta_usedw   : std_logic_vector(4 downto 0) := (others => '0');
    signal meta_data    : std_logic_vector(127 downto 0);
    signal meta_write   : std_logic;

    signal packet_ready : std_logic;
    signal ovf_led      : std_logic;
    signal ovf_count    : unsigned(63 downto 0);

    -- Link epoch stimulus: real firmware always signals a new epoch via
    -- link_start_toggle before raising enable (see fifo_writer's
    -- latch_usb_speed comments). Establishing it here keeps this bench out
    -- of the "enable rose with no epoch" sticky protocol-error/abort path
    -- added in Stage 3, which is exercised by fifo_writer_abort_tb instead.
    signal link_start_toggle : std_logic := '0';
    signal link_stop_toggle   : std_logic := '0';
    signal clear_fault_toggle : std_logic := '0';
    signal fault_sticky       : std_logic_vector(4 downto 0);
    signal abort_active       : std_logic;

    -- Observation counters
    signal cycles          : natural := 0;
    signal writes_total    : natural := 0;
    signal writes_after    : natural := 0;   -- sample writes after the toggle
    signal metas_total     : natural := 0;
    signal metas_after     : natural := 0;
    signal writes_at_meta  : natural := 0;   -- samples seen since last meta
    signal orphan_metas    : natural := 0;   -- meta with no samples before it
    signal toggled         : boolean := false;

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
            link_stop_toggle      => link_stop_toggle,
            clear_fault_toggle    => clear_fault_toggle,
            fault_sticky          => fault_sticky,
            abort_active          => abort_active
        );

    -- Free-running sample timestamp, same shape as time_tamer.
    ts_proc : process(clock)
    begin
        if rising_edge(clock) and reset = '0' then
            timestamp <= timestamp + 1;
        end if;
    end process;

    -- Sample source: valid every cycle while the channel is enabled.
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

    -- Observation: count writes, and flag any metadata header emitted before
    -- any sample of its burst was written.
    obs_proc : process(clock)
    begin
        if rising_edge(clock) and reset = '0' then
            cycles <= cycles + 1;

            if fifo_write = '1' then
                writes_total   <= writes_total + 1;
                writes_at_meta <= writes_at_meta + 1;
                if toggled then
                    writes_after <= writes_after + 1;
                end if;
            end if;

            if meta_write = '1' then
                metas_total <= metas_total + 1;
                if toggled then
                    metas_after <= metas_after + 1;
                end if;
                -- Only headers written after the live toggle are interesting.
                -- The very first header of a stream legitimately precedes its
                -- samples: the writer emits the metadata for a burst before
                -- the burst's first sample write.
                if writes_at_meta = 0 and toggled then
                    orphan_metas <= orphan_metas + 1;
                    report "orphan meta at cycle " & integer'image(cycles) &
                           "  metas_total=" & integer'image(metas_total)
                        severity note;
                end if;
                writes_at_meta <= 0;
            end if;
        end if;
    end process;

    stim : process
        variable writes_before_toggle : natural;
    begin
        reset        <= '1';
        sample_ctrls <= (others => SAMPLE_CONTROL_DISABLE);
        wait for 10*CLK_PERIOD;
        wait until rising_edge(clock);
        reset <= '0';
        wait until rising_edge(clock);

        -- Establish a link epoch before enable, as real firmware does, so
        -- this bench stays out of the Stage 3 "enable with no epoch" sticky
        -- protocol-error/abort path (covered separately by
        -- fifo_writer_abort_tb).
        link_start_toggle <= not link_start_toggle;
        wait until rising_edge(clock);

        -- Bring both channels up: MIMO, which is what the patch guards.
        for i in 0 to NSTREAMS-1 loop
            sample_ctrls(i).enable   <= '1';
            sample_ctrls(i).data_req <= '1';
        end loop;
        enable <= '1';

        -- Let the stream settle and the metadata machinery run at least once.
        for i in 1 to TOGGLE_AT loop
            wait until rising_edge(clock);
        end loop;

        writes_before_toggle := writes_total;

        -- Live toggle: drop channel 1 mid-stream, exactly what software does
        -- when it reconfigures the AD9361 channel enables during operation.
        toggled <= true;
        sample_ctrls(1).enable <= '0';
        for i in 1 to 40 loop
            wait until rising_edge(clock);
        end loop;

        -- Bring it back.
        sample_ctrls(1).enable <= '1';
        for i in 1 to 400 loop
            wait until rising_edge(clock);
        end loop;

        assert writes_after > 0
            report "writer stopped after a live enable toggle at offset " &
                   integer'image(TOGGLE_AT) &
                   " (writes before = " & integer'image(writes_before_toggle) &
                   ", after = " & integer'image(writes_after) & ")"
            severity failure;

        assert orphan_metas = 0
            report "metadata header emitted with no samples in its burst: " &
                   integer'image(orphan_metas) & " orphan(s)"
            severity failure;

        -- Quiescing: both channels off must stop sample writes.
        for i in 0 to NSTREAMS-1 loop
            sample_ctrls(i).enable <= '0';
        end loop;
        for i in 1 to 60 loop
            wait until rising_edge(clock);
        end loop;

        writes_before_toggle := writes_total;
        for i in 1 to 200 loop
            wait until rising_edge(clock);
        end loop;

        assert writes_total = writes_before_toggle
            report "writer kept writing " &
                   integer'image(writes_total - writes_before_toggle) &
                   " samples after both channels were disabled"
            severity failure;

        report "TOGGLE_AT = " & integer'image(TOGGLE_AT) &
               "  writes = " & integer'image(writes_total) &
               "  metas = " & integer'image(metas_total) &
               "  writes_after = " & integer'image(writes_after) &
               "  orphan_metas = " & integer'image(orphan_metas);
        report "live enable toggle: writer kept progress, metadata paired";

        done <= true;
        wait;
    end process;

end architecture;
