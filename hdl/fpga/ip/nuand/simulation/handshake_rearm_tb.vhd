-- Does a handshake crossing take a SECOND value, or only the first?
--
-- Run (ghdl rejects the PRESERVE attribute on a port in the project's
-- synthesis/synchronizer.vhd, so a behavioural two-flop model of it is
-- needed; the protocol under test is handshake's, not the synchroniser's):
--
--     ghdl -a --std=08 <sync model> ../synthesis/handshake.vhd \
--                      handshake_rearm_tb.vhd
--     ghdl -e --std=08 handshake_rearm_tb && ghdl -r --std=08 handshake_rearm_tb
--
-- Why this exists: dwell_cfg drove dest_req from a constant '1'. handshake
-- clears source_ack only when source_req falls (handshake.vhd:68), so with
-- the request held high the crossing loaded source_holding once after reset
-- and never again -- the host could program the dwell threshold exactly
-- once, and every later write was silently ignored. Reading the RTL says
-- so; this measures it rather than trusting the reading.
--
-- Reading the RTL says it cannot: source_ack clears only when source_req
-- goes low, and source_req is the synchronised dest_req, which is tied to
-- '1' at the dwell_cfg instantiation. That predicts exactly one capture
-- after reset. This checks the prediction instead of trusting it.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity handshake_rearm_tb is
end entity;

architecture sim of handshake_rearm_tb is
    signal sclk, dclk, srst, drst : std_logic := '0';
    signal sdata, ddata : std_logic_vector(31 downto 0) := (others => '0');
    signal dack : std_logic;
    signal dreq : std_logic := '0';
    signal captured : std_logic_vector(31 downto 0) := (others => '0');
    signal done : boolean := false;
begin
    sclk <= not sclk after 5 ns when not done else '0';
    dclk <= not dclk after 7 ns when not done else '0';

    U : entity work.handshake
        generic map ( DATA_WIDTH => 32 )
        port map (
            source_reset => srst, source_clock => sclk, source_data => sdata,
            dest_reset => drst, dest_clock => dclk, dest_data => ddata,
            dest_req => dreq, dest_ack => dack
        );

    -- Exactly the req/ack cycle and capture register added to bladerf_core.
    drive : process( dclk, drst )
    begin
        if( drst = '1' ) then
            dreq <= '0';
        elsif( rising_edge(dclk) ) then
            if( dack = '0' ) then
                dreq <= '1';
            else
                dreq <= '0';
            end if;
        end if;
    end process;

    capture : process( dclk, drst )
    begin
        if( drst = '1' ) then
            captured <= (others => '0');
        elsif( rising_edge(dclk) ) then
            if( dack = '1' ) then
                captured <= ddata;
            end if;
        end if;
    end process;

    stim : process
    begin
        srst <= '1'; drst <= '1';
        sdata <= x"AAAA0001";
        wait for 100 ns;
        srst <= '0'; drst <= '0';
        wait for 300 ns;

        report "after reset, captured = " & to_hstring(captured);
        assert captured = x"AAAA0001"
            report "first value never crossed" severity failure;

        -- Host reprogrammes. This is the case that matters: config changes
        -- between dwells for the whole life of the design.
        sdata <= x"BBBB0002";
        wait for 500 ns;
        report "after reprogramme, captured = " & to_hstring(captured);

        if captured = x"BBBB0002" then
            report "SECOND VALUE CROSSED -- handshake does re-arm";
        else
            report "SECOND VALUE NEVER CROSSED -- stuck at " & to_hstring(captured)
                severity failure;
        end if;

        done <= true;
        wait;
    end process;
end architecture;
