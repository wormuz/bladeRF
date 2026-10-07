library ieee;
use ieee.std_logic_1164.all;
use std.env.all;

entity sticky_reduce_tb is
end entity;

architecture test of sticky_reduce_tb is
    signal clock       : std_logic := '0';
    signal reset       : std_logic := '1';
    signal set_fault   : std_logic := '0';
    signal clear_fault : std_logic := '0';
    signal sticky      : std_logic_vector(4 downto 0) := (others => '0');
    signal any_set     : std_logic;
begin
    clock <= not clock after 5 ns;

    -- Model the source-domain sticky register that feeds the reducer.
    source_flags : process(clock, reset)
    begin
        if reset = '1' then
            sticky <= (others => '0');
        elsif rising_edge(clock) then
            if clear_fault = '1' then
                sticky <= (others => '0');
            elsif set_fault = '1' then
                sticky(2) <= '1';
            end if;
        end if;
    end process;

    dut : entity work.sticky_reduce
        generic map ( WIDTH => sticky'length )
        port map ( clock => clock, reset => reset,
                   flags => sticky, any_set => any_set );

    check : process
    begin
        wait for 12 ns;
        reset <= '0';
        wait until rising_edge(clock);
        set_fault <= '1';
        wait until rising_edge(clock);
        set_fault <= '0';
        wait for 1 ns;
        assert sticky(2) = '1' and any_set = '0'
            report "aggregate must wait one source edge after sticky set"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert any_set = '1'
            report "registered aggregate missed a sticky fault" severity failure;

        clear_fault <= '1';
        wait until rising_edge(clock);
        clear_fault <= '0';
        wait for 1 ns;
        assert sticky = "00000" and any_set = '1'
            report "clear must propagate only after the source sticky clears"
            severity failure;
        wait until rising_edge(clock);
        wait for 1 ns;
        assert any_set = '0'
            report "registered aggregate did not follow explicit clear"
            severity failure;

        report "sticky_reduce_tb: PASS" severity note;
        finish;
    end process;
end architecture;
