library ieee;
use ieee.numeric_std.all;

package time_tamer_timestamp_p is
    -- Timestamp ordering is unambiguous when a scheduled time is less than
    -- 2^63 ticks ahead of the current 64-bit counter. This modulo comparison
    -- handles counter rollover; the exactly-half-range case is not reached.
    function timestamp_has_reached(
        timestamp : unsigned(63 downto 0);
        target    : unsigned(63 downto 0)
    ) return boolean;
end package;

package body time_tamer_timestamp_p is
    function timestamp_has_reached(
        timestamp : unsigned(63 downto 0);
        target    : unsigned(63 downto 0)
    ) return boolean is
    begin
        return signed(timestamp - target) >= 0;
    end function;
end package body;
