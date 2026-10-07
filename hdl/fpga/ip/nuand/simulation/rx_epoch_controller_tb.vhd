library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;
    use std.env.all;

library work;
    use work.fifo_readwrite_p.all;

entity rx_epoch_controller_tb is
end entity;

architecture test of rx_epoch_controller_tb is
    signal source_clock : std_logic := '0';
    signal rx_clock : std_logic := '0';
    signal source_reset : std_logic := '1';
    signal rx_reset : std_logic := '1';
    signal source_control : std_logic_vector(31 downto 0) := (others => '0');
    signal rx_control : std_logic_vector(31 downto 0);
    signal arm, complete, abort : std_logic;
    signal epoch_id : unsigned(7 downto 0);
    signal meta_enable, complete_seen : std_logic;
    signal controls_in : sample_controls_t(0 to 1) :=
        (others => SAMPLE_CONTROL_ENABLE);
    signal samples_in : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal timestamp : unsigned(63 downto 0) := (others => '0');
    signal controls_out : sample_controls_t(0 to 1);
    signal samples_out : sample_streams_t(0 to 1);
    signal timestamp_out : unsigned(63 downto 0);
    signal gate_epoch_id : unsigned(7 downto 0);
    signal gate_state : unsigned(3 downto 0);
    signal discard_active, start_event : std_logic;
    signal first_valid_timestamp : unsigned(63 downto 0);
begin
    source_clock <= not source_clock after 5 ns;
    rx_clock <= not rx_clock after 7 ns;

    dut : entity work.rx_epoch_controller
        port map (
            source_clock => source_clock,
            source_reset => source_reset,
            source_control => source_control,
            rx_clock => rx_clock,
            rx_reset => rx_reset,
            rx_control => rx_control,
            epoch_arm => arm,
            epoch_complete => complete,
            epoch_abort => abort,
            epoch_id => epoch_id,
            meta_enable => meta_enable,
            complete_seen => complete_seen
        );

    gate : entity work.rx_epoch_gate
        port map (
            clock => rx_clock,
            reset => rx_reset,
            in_sample_controls => controls_in,
            in_samples => samples_in,
            rx_timestamp => timestamp,
            epoch_arm => arm,
            epoch_complete => complete,
            epoch_abort => abort,
            epoch_id_in => epoch_id,
            out_sample_controls => controls_out,
            out_samples => samples_out,
            out_timestamp => timestamp_out,
            out_epoch_id => gate_epoch_id,
            out_state => gate_state,
            out_discard_active => discard_active,
            epoch_start_event => start_event,
            first_valid_timestamp => first_valid_timestamp
        );

    test_process : process
        procedure issue(control : std_logic_vector(31 downto 0);
                        expected_arm, expected_complete, expected_abort : std_logic) is
        begin
            source_control <= control;
            if expected_arm = '1' then
                wait until arm = '1';
            elsif expected_complete = '1' then
                wait until complete = '1';
            elsif expected_abort = '1' then
                wait until abort = '1';
            else
                wait for 1 ns;
                assert false report "test command has no expected toggle" severity failure;
            end if;
            wait for 1 ns;
            assert arm = expected_arm and complete = expected_complete and
                   abort = expected_abort
                report "controller decoded an unexpected/stale toggle" severity failure;
        end procedure;

        variable command_word : std_logic_vector(31 downto 0) := (others => '0');
    begin
        wait for 50 ns;
        wait until falling_edge(rx_clock);
        source_reset <= '0';
        rx_reset <= '0';

        -- Arm epoch 0x31 through the actual asynchronous bundled handshake.
        -- The ID and toggles must arrive together at the gate.
        command_word(0) := '1';
        command_word(15 downto 8) := x"31";
        command_word(16) := '1';
        issue(command_word, '1', '0', '0');
        wait until gate_state = "0001";
        assert gate_epoch_id = x"31" and meta_enable = '1' and
               rx_control = command_word
            report "ARM did not carry the matching ID/control word" severity failure;

        command_word(1) := '1';
        issue(command_word, '0', '1', '0');
        wait until gate_state = "0010";
        if complete_seen /= '1' then
            wait until complete_seen = '1';
        end if;
        timestamp <= to_unsigned(300, timestamp'length);
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '1';
        samples_in(0).data_i <= to_signed(1234, 16);
        samples_in(1).data_i <= to_signed(5678, 16);
        wait until start_event = '1';
        wait for 1 ns;
        assert start_event = '1' and gate_epoch_id = x"31" and
               first_valid_timestamp = to_unsigned(300, timestamp'length) and
               timestamp_out = to_unsigned(300, timestamp'length) and
               samples_out(0).data_i = to_signed(1234, 16) and
               samples_out(1).data_i = to_signed(5678, 16)
            report "first paired IQ did not open at the matching sample timestamp"
            severity failure;
        wait until gate_state = "0000";

        -- Abort the active epoch, then rearm with a different ID. Stale
        -- COMPLETE/ABORT toggle levels must not replay across handshake polls.
        command_word(2) := '1';
        issue(command_word, '0', '0', '1');
        wait until gate_state = "0100";
        wait for 1 ns;
        assert discard_active = '1' and samples_out(0).data_i = to_signed(0, 16) and
               samples_out(1).data_i = to_signed(0, 16)
            report "ABORT did not fence both IQ lanes" severity failure;

        samples_in(0).data_v <= '0';
        samples_in(1).data_v <= '0';
        command_word(0) := '0';
        command_word(15 downto 8) := x"32";
        issue(command_word, '1', '0', '0');
        wait until gate_state = "0001";
        assert gate_epoch_id = x"32"
            report "recovery ARM lost its new epoch ID" severity failure;

        command_word(1) := '0';
        issue(command_word, '0', '1', '0');
        wait until gate_state = "0010";
        if complete_seen /= '1' then
            wait until complete_seen = '1';
        end if;
        timestamp <= to_unsigned(900, timestamp'length);
        samples_in(0).data_v <= '1';
        samples_in(1).data_v <= '1';
        samples_in(0).data_i <= to_signed(4321, 16);
        samples_in(1).data_i <= to_signed(8765, 16);
        wait until start_event = '1';
        wait for 1 ns;
        assert start_event = '1' and gate_epoch_id = x"32" and
               first_valid_timestamp = to_unsigned(900, timestamp'length) and
               timestamp_out = to_unsigned(900, timestamp'length) and
               samples_out(0).data_i = to_signed(4321, 16) and
               samples_out(1).data_i = to_signed(8765, 16)
            report "ABORT recovery admitted IQ without a fresh paired boundary"
            severity failure;

        report "rx_epoch_controller_tb: PASS" severity note;
        stop;
        wait;
    end process;
end architecture;
