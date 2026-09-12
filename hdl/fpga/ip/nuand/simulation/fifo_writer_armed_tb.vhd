-- fifo_writer: enable-before-START is the legitimate order (ARMED state).
--
-- Stock FX3 firmware resets the fabric and raises the datapath enable
-- inside one vendor command, so the host can only ever declare the epoch
-- AFTER enable is high. The writer must therefore treat "enable high, no
-- epoch" as a hold, not a violation.
--
--   1. enable rises with no epoch: no violation, no sticky fault, no
--      abort, link inactive, FIFO held in clear, nothing written.
--   2. START arrives while enabled: link comes up, clear releases,
--      samples flow.
--   3. enable low, STOP, then a START with enable low: ignored (shared
--      START; the unused direction sees it every session); re-enable and
--      the next START is taken.
--   4. disabled on a live epoch: the progress watchdog stays quiet.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_writer_armed_tb is
end entity;

architecture tb of fifo_writer_armed_tb is

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

    signal link_start_toggle  : std_logic := '1';  -- held high through reset: a stale '1' on the synchronised toggle is what the DUT sees after an FX3 fabric reset
    signal link_stop_toggle   : std_logic := '1';  -- likewise
    signal clear_fault_toggle : std_logic := '0';
    signal usb_speed_mismatch : std_logic;
    signal link_active        : std_logic;
    signal speed_latched      : std_logic;
    signal protocol_start_violation : std_logic;
    signal link_epoch_counter : unsigned(7 downto 0);
    signal fault_sticky       : std_logic_vector(4 downto 0);
    signal abort_active       : std_logic;

    -- Counted by a separate process so the stimulus can ask "did anything
    -- get written during that window" without sampling every clock itself.
    signal writes_seen  : natural := 0;

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

    -- Continuous data, as the ADC interface delivers it: valid every clock
    -- from the moment the stream is enabled. Discarding it in ARMED is the
    -- intended behaviour, not a fault.
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

    count_proc : process(clock)
    begin
        if rising_edge(clock) and fifo_write = '1' then
            writes_seen <= writes_seen + 1;
        end if;
    end process;

    stim : process
        variable writes_at_start : natural;
    begin
        reset <= '1';
        wait for 10*CLK_PERIOD;
        wait until rising_edge(clock);
        reset <= '0';
        wait until rising_edge(clock);

        for i in 0 to NSTREAMS-1 loop
            sample_ctrls(i).enable   <= '1';
            sample_ctrls(i).data_req <= '1';
        end loop;

        ------------------------------------------------------------------
        -- 1. enable first, no epoch: ARMED.
        ------------------------------------------------------------------
        enable <= '1';
        for i in 1 to 200 loop
            wait until rising_edge(clock);
        end loop;

        assert protocol_start_violation = '0'
            report "case 1: enable before START flagged as a violation -- "
                 & "with stock FX3 this is the only order the host can achieve"
            severity error;
        assert fault_sticky = "00000"
            report "case 1: sticky fault set while merely armed"
            severity error;
        assert abort_active = '0'
            report "case 1: abort asserted while merely armed"
            severity error;
        assert link_active = '0'
            report "case 1: link reported active without any START"
            severity error;
        assert fifo_clear = '1'
            report "case 1: FIFO not held in clear while armed -- the first "
                 & "sample of the epoch would be a stale one"
            severity error;
        assert writes_seen = 0
            report "case 1: " & integer'image(writes_seen)
                 & " samples written before any epoch"
            severity error;
        report "case 1 OK: enable without epoch is ARMED -- no fault, FIFO "
             & "held clear, nothing written";

        ------------------------------------------------------------------
        -- 2. START while enabled: stream.
        ------------------------------------------------------------------
        link_start_toggle <= not link_start_toggle;
        for i in 1 to 200 loop
            wait until rising_edge(clock);
        end loop;

        assert link_active = '1'
            report "case 2: link did not come up on START"
            severity error;
        assert fifo_clear = '0'
            report "case 2: FIFO still held in clear after START"
            severity error;
        assert writes_seen > 0
            report "case 2: no samples written after START"
            severity error;
        assert fault_sticky = "00000"
            report "case 2: sticky fault after a clean START"
            severity error;
        report "case 2 OK: START releases ARMED, " & integer'image(writes_seen)
             & " samples written";

        ------------------------------------------------------------------
        -- 3. START while disabled is IGNORED: the START toggle is shared by
        --    both directions, so the unused one sees it in every session.
        --    No link, no fault, no writes -- and the next START, once
        --    enabled again, is a fresh edge.
        ------------------------------------------------------------------
        -- Disable, then STOP -- the host's order. enable alone does not end
        -- the epoch (link_active is epoch state, STOP drops it); STOP also
        -- sets the FIFO_ABORT sticky bit (4) by design.
        enable <= '0';
        for i in 1 to 20 loop
            wait until rising_edge(clock);
        end loop;
        link_stop_toggle <= not link_stop_toggle;
        for i in 1 to 20 loop
            wait until rising_edge(clock);
        end loop;
        assert link_active = '0'
            report "case 3: STOP did not drop link_active"
            severity error;
        writes_at_start := writes_seen;

        link_start_toggle <= not link_start_toggle;
        for i in 1 to 50 loop
            wait until rising_edge(clock);
        end loop;

        assert protocol_start_violation = '0'
            report "case 3: START with enable low flagged as a violation -- "
                 & "the unused direction of a shared START would fault every session"
            severity error;
        assert fault_sticky(3) = '0'
            report "case 3: protocol fault set by a START on a disabled direction"
            severity error;
        assert link_active = '0'
            report "case 3: link came up on a disabled direction -- START must "
                 & "be ignored while enable is low"
            severity error;
        assert writes_seen = writes_at_start
            report "case 3: samples written with enable low"
            severity error;

        enable <= '1';
        for i in 1 to 20 loop
            wait until rising_edge(clock);
        end loop;
        link_start_toggle <= not link_start_toggle;
        for i in 1 to 50 loop
            wait until rising_edge(clock);
        end loop;
        assert link_active = '1'
            report "case 3: START after re-enable not accepted as a fresh edge"
            severity error;
        report "case 3 OK: START while disabled ignored; next START taken";

        ------------------------------------------------------------------
        -- 4. Disabled with the epoch still up (the other direction keeps
        --    streaming, so no STOP): the progress watchdog must not call
        --    this a stall, now or when enable returns.
        ------------------------------------------------------------------
        enable <= '0';
        -- One wait, not 4M iterations of "wait until": GHDL mcode takes
        -- minutes on the loop and seconds on the single delay.
        wait for (2**22 + 200) * CLK_PERIOD;
        wait until rising_edge(clock);
        assert fault_sticky(1) = '0' and fault_sticky(2) = '0'
            report "case 4: progress fault raised on a disabled direction "
                 & "whose epoch is still up (rx_fault=1 at TX disable)"
            severity error;

        writes_at_start := writes_seen;
        enable <= '1';
        for i in 1 to 300 loop
            wait until rising_edge(clock);
        end loop;
        assert fault_sticky(1) = '0' and fault_sticky(2) = '0'
            report "case 4: progress fault fired the instant enable returned "
                 & "-- the watchdog counted while disabled"
            severity error;
        assert writes_seen > writes_at_start
            report "case 4: no writes after re-enable on a live epoch"
            severity error;
        report "case 4 OK: disabled direction holds its watchdog";

        report "fifo_writer_armed_tb: all cases passed";
        done <= true;
        wait;
    end process;

end architecture;
