library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity tx_retune_gate_controller_tb is
end entity;

architecture test of tx_retune_gate_controller_tb is
    signal source_clock : std_logic := '0';
    signal tx_clock : std_logic := '0';
    signal tx_clock_enable : std_logic := '1';
    signal source_reset : std_logic := '1';
    signal tx_reset : std_logic := '1';
    signal control : std_logic_vector(31 downto 0) := (others => '0');
    signal target_lo : std_logic_vector(31 downto 0) := (others => '0');
    signal target_hi : std_logic_vector(31 downto 0) := (others => '0');
    signal arm : std_logic;
    signal immediate : std_logic;
    signal arm_ack_toggle : std_logic;
    signal target : unsigned(63 downto 0);
    signal complete : std_logic;
    signal abort : std_logic;
    signal fifo_flush_sys : std_logic;
    signal fifo_release_sys : std_logic;
begin
    source_clock <= not source_clock after 5 ns;
    tx_clock <= not tx_clock after 7 ns when tx_clock_enable = '1' else '0';

    dut : entity work.tx_retune_gate_controller
        port map (
            source_clock => source_clock,
            source_reset => source_reset,
            source_control => control,
            source_target_lo => target_lo,
            source_target_hi => target_hi,
            tx_clock => tx_clock,
            tx_reset => tx_reset,
            arm => arm,
            arm_immediate => immediate,
            arm_ack_toggle => arm_ack_toggle,
            target_timestamp => target,
            complete => complete,
            abort => abort,
            fifo_flush_sys => fifo_flush_sys,
            fifo_release_sys => fifo_release_sys
        );

    stimulus : process
        procedure wait_for(signal value : in std_logic; expected : std_logic) is
            variable found : boolean := false;
        begin
            for i in 0 to 100 loop
                wait until rising_edge(tx_clock);
                wait for 1 ns;
                if value = expected then
                    found := true;
                    exit;
                end if;
            end loop;
            assert found report "TX gate CDC command was lost" severity failure;
        end procedure;

        procedure wait_for_source(signal value : in std_logic;
                                  expected : std_logic) is
            variable found : boolean := false;
        begin
            for i in 0 to 100 loop
                wait until rising_edge(source_clock);
                wait for 1 ns;
                if value = expected then
                    found := true;
                    exit;
                end if;
            end loop;
            assert found report "system-clock fault command was lost"
                severity failure;
        end procedure;

        procedure wait_for_compare_arm(
            expected_target : unsigned(63 downto 0)) is
            variable captured : boolean := false;
        begin
            for i in 0 to 100 loop
                wait until rising_edge(tx_clock);
                wait for 1 ns;
                if target = expected_target then
                    captured := true;
                    exit;
                end if;
            end loop;
            assert captured report "TX target was not captured" severity failure;
            assert arm = '0'
                report "TX ARM escaped before compare pipeline lead" severity failure;
            wait until rising_edge(tx_clock);
            wait for 1 ns;
            assert arm = '0'
                report "TX ARM escaped after only one compare lead cycle"
                severity failure;
            wait until rising_edge(tx_clock);
            wait for 1 ns;
            assert arm = '1'
                report "TX ARM was not issued after compare pipeline lead"
                severity failure;
        end procedure;
    begin
        -- Model the NIOS PIO retaining command toggles across FX3 SYS_RST.
        control(5 downto 0) <= "111111";
        wait for 50 ns;
        source_reset <= '0';
        tx_reset <= '0';
        wait for 200 ns;
        assert arm = '0' and complete = '0' and abort = '0' and
               arm_ack_toggle = '1'
            report "stale TX command toggle replayed after reset"
            severity failure;

        target_lo <= x"89abcdef";
        target_hi <= x"01234567";
        control(3) <= '0';
        control(0) <= '0';
        wait_for_compare_arm(unsigned'(x"0123456789abcdef"));
        assert std_logic_vector(target) = x"0123456789abcdef"
            report "TX gate target was torn across CDC" severity failure;
        assert immediate = '0' report "future ARM became immediate" severity failure;
        assert complete = '0' and abort = '0'
            report "target timestamp bits generated control commands"
            severity failure;
        assert arm_ack_toggle = '0'
            report "ARM acknowledgement did not match the captured toggle"
            severity failure;
        wait_for(arm, '0');

        target_lo <= x"00000002";
        target_hi <= x"00000003";
        control(0) <= '1';
        wait_for_compare_arm(unsigned'(x"0000000300000002"));
        assert std_logic_vector(target) = x"0000000300000002" and
               arm_ack_toggle = '1'
            report "back-to-back ARM acknowledgement was stale"
            severity failure;
        assert immediate = '0' and complete = '0' and abort = '0'
            report "second target timestamp corrupted control state"
            severity failure;
        wait_for(arm, '0');

        control(1) <= '0';
        wait_for(complete, '1');
        wait_for(complete, '0');
        control(2) <= '0';
        wait_for(abort, '1');
        wait_for(abort, '0');

        -- Fault-flush commands remain decodable in sys_clock after TX clock
        -- loss; ordinary gate commands above deliberately used tx_clock.
        tx_clock_enable <= '0';
        wait for 30 ns;
        control(4) <= '0';
        wait_for_source(fifo_flush_sys, '1');
        wait_for_source(fifo_flush_sys, '0');
        control(5) <= '0';
        wait_for_source(fifo_release_sys, '1');
        wait_for_source(fifo_release_sys, '0');
        report "TX retune and independent fault command CDC checks complete"
            severity note;
        wait;
    end process;
end architecture;
