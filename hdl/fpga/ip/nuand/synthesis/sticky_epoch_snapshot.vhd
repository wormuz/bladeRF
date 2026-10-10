-- Retain the aggregate fault state immediately before a shared link START.
-- The live per-epoch sticky vector is cleared by START; this register gives
-- software one complete epoch to observe the fault that caused the previous
-- link generation to be invalid.
library ieee;
use ieee.std_logic_1164.all;

entity sticky_epoch_snapshot is
    generic (
        WIDTH : positive := 1
    );
    port (
        clock        : in  std_logic;
        reset        : in  std_logic;
        start_toggle : in  std_logic;
        sticky_flags : in  std_logic_vector(WIDTH-1 downto 0);
        previous_fault : out std_logic := '0'
    );
end entity;

architecture rtl of sticky_epoch_snapshot is
    signal start_toggle_prev : std_logic := '0';
    signal previous_fault_i  : std_logic := '0';
begin
    process(clock, reset)
        variable any_fault : std_logic;
    begin
        if reset = '1' then
            start_toggle_prev <= '0';
            previous_fault_i <= '0';
        elsif rising_edge(clock) then
            if start_toggle /= start_toggle_prev then
                any_fault := '0';
                for i in sticky_flags'range loop
                    any_fault := any_fault or sticky_flags(i);
                end loop;
                previous_fault_i <= any_fault;
            end if;
            start_toggle_prev <= start_toggle;
        end if;
    end process;

    previous_fault <= previous_fault_i;
end architecture;
