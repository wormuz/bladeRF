library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity tx_retune_gate_controller_tb is
end entity;

architecture test of tx_retune_gate_controller_tb is
    signal source_clock : std_logic := '0';
    signal tx_clock : std_logic := '0';
    signal source_reset : std_logic := '1';
    signal tx_reset : std_logic := '1';
    signal control : std_logic_vector(31 downto 0) := (others => '0');
    signal target_lo : std_logic_vector(31 downto 0) := (others => '0');
    signal target_hi : std_logic_vector(31 downto 0) := (others => '0');
    signal arm : std_logic;
    signal immediate : std_logic;
    signal target : unsigned(63 downto 0);
    signal complete : std_logic;
    signal abort : std_logic;
begin
    source_clock <= not source_clock after 5 ns;
    tx_clock <= not tx_clock after 7 ns;

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
            target_timestamp => target,
            complete => complete,
            abort => abort
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
    begin
        wait for 50 ns;
        source_reset <= '0';
        tx_reset <= '0';
        wait for 200 ns;

        target_lo <= x"89abcdef";
        target_hi <= x"01234567";
        control(3) <= '0';
        control(0) <= '1';
        wait_for(arm, '1');
        assert std_logic_vector(target) = x"0123456789abcdef"
            report "TX gate target was torn across CDC" severity failure;
        assert immediate = '0' report "future ARM became immediate" severity failure;
        wait_for(arm, '0');

        control(1) <= '1';
        wait_for(complete, '1');
        wait_for(complete, '0');
        control(2) <= '1';
        wait_for(abort, '1');
        report "TX gate controller CDC checks complete" severity note;
        wait;
    end process;
end architecture;
