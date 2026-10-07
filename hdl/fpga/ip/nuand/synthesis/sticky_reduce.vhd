-- Copyright (c) 2017 Nuand LLC
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
--
-- Register source-domain sticky fault flags into a single bit before crossing
-- clock domains. This adds one source-clock cycle of assertion latency, but
-- cannot lose a fault held until an explicit clear.
library ieee;
    use ieee.std_logic_1164.all;

entity sticky_reduce is
    generic (
        WIDTH : positive := 1
    );
    port (
        clock  : in  std_logic;
        reset  : in  std_logic;
        flags  : in  std_logic_vector(WIDTH-1 downto 0);
        any_set: out std_logic := '0'
    );
end entity;

architecture rtl of sticky_reduce is
begin
    process(clock, reset)
        variable flags_any : std_logic;
    begin
        if reset = '1' then
            any_set <= '0';
        elsif rising_edge(clock) then
            flags_any := '0';
            for i in flags'range loop
                flags_any := flags_any or flags(i);
            end loop;
            any_set <= flags_any;
        end if;
    end process;
end architecture;
