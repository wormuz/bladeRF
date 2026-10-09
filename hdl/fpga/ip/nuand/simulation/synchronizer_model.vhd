-- Behavioral two-flop synchronizer for GHDL testbenches. The production RTL
-- carries Quartus PRESERVE attributes on an output port that GHDL rejects;
-- this model keeps the reset and sampling behavior needed by CDC tests.
library ieee;
use ieee.std_logic_1164.all;

entity synchronizer is
    generic (
        RESET_LEVEL : std_logic := '1'
    );
    port (
        reset  : in  std_logic;
        clock  : in  std_logic;
        async  : in  std_logic;
        sync   : out std_logic := RESET_LEVEL
    );
end entity;

architecture model of synchronizer is
    signal reg0, reg1 : std_logic := RESET_LEVEL;
begin
    process (clock, reset)
    begin
        if reset = '1' then
            reg0 <= RESET_LEVEL;
            reg1 <= RESET_LEVEL;
            sync <= RESET_LEVEL;
        elsif rising_edge(clock) then
            reg0 <= async;
            reg1 <= reg0;
            sync <= reg1;
        end if;
    end process;
end architecture;
