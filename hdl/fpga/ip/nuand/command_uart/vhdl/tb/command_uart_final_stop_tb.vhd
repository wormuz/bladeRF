-- Response commit at the final stop-bit boundary of the active frame.
--
-- A 16-byte response frame is on the wire; a second response is committed at
-- every clock around the end of the last stop bit. Whatever the alignment, the
-- second frame must arrive complete, once, byte-exact, after the first, with
-- no overflow. The boundary alignment itself is identified from the design:
-- the commit is consumed directly (the pending slot never fills) and the
-- serializer never goes idle between the two frames.
--
-- Chain case: with the pending slot holding B, a third commit C around the same
-- boundary is either accepted (A,B,C in order, no overflow) or refused with the
-- sticky overflow (A,B in order, C absent). Never reordered, duplicated,
-- truncated, and acceptance is monotonic in the commit time.

library ieee ;
    use ieee.std_logic_1164.all ;
    use ieee.numeric_std.all ;

library std ;
    use std.env.all ;

entity command_uart_final_stop_tb is
end entity ;

architecture arch of command_uart_final_stop_tb is

    constant CLOCKS_PER_BIT : natural := 20 ;
    constant FRAME_CLOCKS   : natural := 10 * CLOCKS_PER_BIT ;
    constant SWEEP          : integer := 40 ;

    signal clock : std_logic := '1' ;
    signal reset : std_logic := '1' ;
    signal clk_n : natural := 0 ;

    signal sout  : std_logic ;
    signal addr  : std_logic_vector(4 downto 0) := (others => '0') ;
    signal dout  : std_logic_vector(31 downto 0) ;
    signal din   : std_logic_vector(31 downto 0) := (others => '0') ;
    signal rd    : std_logic := '0' ;
    signal wr    : std_logic := '0' ;
    signal waitreq, readack, irq : std_logic ;

    type byte_array_t is array (0 to 8191) of std_logic_vector(7 downto 0) ;
    signal rx_buf : byte_array_t ;
    signal rx_count : natural := 0 ;
    signal bytes_started : natural := 0 ;
    signal start_clk : natural := 0 ;

    signal pending_rises : natural := 0 ;
    signal active_falls  : natural := 0 ;


    function frame(base : natural) return std_logic_vector is
        variable v : std_logic_vector(127 downto 0) ;
    begin
        for j in 0 to 15 loop
            v(j*8+7 downto j*8) := std_logic_vector(to_unsigned((base + j) mod 256, 8)) ;
        end loop ;
        return v ;
    end function ;

    constant A : std_logic_vector(127 downto 0) := frame(16#10#) ;
    constant B : std_logic_vector(127 downto 0) := frame(16#50#) ;
    constant C : std_logic_vector(127 downto 0) := frame(16#90#) ;

begin

    clock <= not clock after 5 ns ;

    clk_count : process(clock)
    begin
        if rising_edge(clock) then
            clk_n <= clk_n + 1 ;
        end if ;
    end process ;

    dut : entity work.command_uart
        port map (
            clock => clock, reset => reset,
            rs232_sout => sout, rs232_sin => '1',
            addr => addr, dout => dout, din => din,
            read => rd, write => wr,
            waitreq => waitreq, readack => readack, irq => irq
        ) ;

    -- Receive the response line.
    rx_monitor : process
        variable v : std_logic_vector(7 downto 0) ;
        variable n : natural := 0 ;
        variable started : natural := 0 ;
    begin
        wait until falling_edge(sout) ;
        started := started + 1 ;
        start_clk <= clk_n ;
        bytes_started <= started ;
        for i in 1 to CLOCKS_PER_BIT * 3 / 2 loop
            wait until rising_edge(clock) ;
        end loop ;
        for bi in 0 to 7 loop
            v(bi) := sout ;
            for i in 1 to CLOCKS_PER_BIT loop
                wait until rising_edge(clock) ;
            end loop ;
        end loop ;
        assert sout = '1' report "stop bit missing on response line"
            severity failure ;
        rx_buf(n) <= v ;
        n := n + 1 ;
        rx_count <= n ;
    end process ;

    pending_counter : process
        alias dut_pending is
            <<signal .command_uart_final_stop_tb.dut.pending_response_valid : std_logic>> ;
    begin
        wait until rising_edge(dut_pending) ;
        pending_rises <= pending_rises + 1 ;
    end process ;

    active_counter : process
        alias dut_active is
            <<signal .command_uart_final_stop_tb.dut.response_active : std_logic>> ;
    begin
        wait until falling_edge(dut_active) ;
        active_falls <= active_falls + 1 ;
    end process ;

    stimulus : process
        variable direct_at_boundary : natural := 0 ;
        variable pending_path       : natural := 0 ;
        variable after_boundary     : natural := 0 ;
        variable chain_refused      : natural := 0 ;
        variable chain_accepted     : natural := 0 ;
        variable seen_accept        : boolean := false ;

        procedure tick(count : natural) is
        begin
            for i in 1 to count loop
                wait until rising_edge(clock) ;
            end loop ;
        end procedure ;

        procedure write_word(offset : natural;
                             value : std_logic_vector(31 downto 0)) is
        begin
            addr <= std_logic_vector(to_unsigned(offset, addr'length)) ;
            din <= value ;
            wr <= '1' ;
            wait until rising_edge(clock) ;
            wr <= '0' ;
            wait until rising_edge(clock) ;
        end procedure ;

        -- Everything except the committing word.
        procedure stage(v : std_logic_vector(127 downto 0)) is
        begin
            write_word(24, x"00000000") ;
            write_word(0, v(31 downto 0)) ;
            write_word(4, v(63 downto 32)) ;
            write_word(8, v(95 downto 64)) ;
        end procedure ;

        procedure fire(v : std_logic_vector(127 downto 0)) is
        begin
            write_word(12, v(127 downto 96)) ;
        end procedure ;

        procedure wait_clk(target : natural) is
        begin
            while clk_n < target loop
                wait until rising_edge(clock) ;
            end loop ;
        end procedure ;

        procedure read_status(s : out std_logic_vector(31 downto 0)) is
        begin
            addr <= std_logic_vector(to_unsigned(28, addr'length)) ;
            rd <= '1' ;
            wait until rising_edge(clock) ;
            wait for 1 ns ;
            s := dout ;
            rd <= '0' ;
            wait until rising_edge(clock) ;
        end procedure ;

        procedure expect_frame(base : natural; v : std_logic_vector(127 downto 0);
                               what : string) is
        begin
            for j in 0 to 15 loop
                assert rx_buf(base + j) = v(j*8+7 downto j*8)
                    report what & ": byte " & integer'image(j) & " is "
                           & to_hstring(rx_buf(base + j)) & ", expected "
                           & to_hstring(v(j*8+7 downto j*8))
                    severity failure ;
            end loop ;
        end procedure ;

        procedure drain_and_reset is
        begin
            tick(1500) ;
            reset <= '1' ;
            tick(4) ;
            reset <= '0' ;
            tick(10) ;
        end procedure ;

        procedure wait_bytes(target : natural; what : string) is
            variable limit : natural := clk_n + 3 * 16 * (FRAME_CLOCKS + 40) ;
        begin
            while rx_count < target and clk_n < limit loop
                wait until rising_edge(clock) ;
            end loop ;
            assert rx_count >= target
                report what & ": response frame lost, " & integer'image(rx_count)
                       & " of " & integer'image(target) & " bytes received"
                severity failure ;
        end procedure ;

        variable base, bs0, t15, p0, f0 : natural ;
        variable s : std_logic_vector(31 downto 0) ;
    begin
        tick(20) ;
        reset <= '0' ;
        tick(20) ;

        -- A active, B committed around A's final stop-bit boundary.
        for d in -SWEEP to SWEEP loop
            base := rx_count ;
            bs0 := bytes_started ;
            p0 := pending_rises ;
            f0 := active_falls ;
            stage(A) ;
            fire(A) ;
            stage(B) ;
            wait_bytes(base + 1, "A first byte") ;
            wait until bytes_started = bs0 + 16 ;
            t15 := start_clk ;
            wait_clk(t15 + FRAME_CLOCKS + d - 2) ;
            fire(B) ;
            wait_bytes(base + 32, "B at offset " & integer'image(d)) ;
            tick(2 * FRAME_CLOCKS) ;
            assert rx_count = base + 32
                report "extra bytes after B at offset " & integer'image(d)
                severity failure ;
            expect_frame(base, A, "A at offset " & integer'image(d)) ;
            expect_frame(base + 16, B, "B at offset " & integer'image(d)) ;
            read_status(s) ;
            assert s(4) = '0' and s(3) = '0' and s(2) = '0'
                report "status after A,B at offset " & integer'image(d)
                       & ": " & to_hstring(s)
                severity failure ;
            if pending_rises = p0 and active_falls = f0 + 1 then
                direct_at_boundary := direct_at_boundary + 1 ;
            elsif pending_rises = p0 + 1 and active_falls = f0 + 1 then
                pending_path := pending_path + 1 ;
            elsif pending_rises = p0 and active_falls = f0 + 2 then
                after_boundary := after_boundary + 1 ;
            else
                report "offset " & integer'image(d) & ": unclassified path, "
                       & "pending rises " & integer'image(pending_rises - p0)
                       & ", idle gaps " & integer'image(active_falls - f0)
                    severity failure ;
            end if ;
            drain_and_reset ;
        end loop ;
        report "single-commit sweep: direct-at-boundary="
               & integer'image(direct_at_boundary)
               & " pending-path=" & integer'image(pending_path)
               & " after-boundary=" & integer'image(after_boundary) ;
        assert direct_at_boundary >= 1
            report "sweep never hit the final stop-bit boundary commit"
            severity failure ;
        assert pending_path >= 1 and after_boundary >= 1
            report "sweep did not cover both sides of the boundary"
            severity failure ;

        -- Chain: B pending behind A, C committed around A's boundary.
        for d in -SWEEP to SWEEP loop
            base := rx_count ;
            bs0 := bytes_started ;
            stage(A) ;
            fire(A) ;
            wait_bytes(base + 1, "A first byte") ;
            stage(B) ;
            fire(B) ;
            stage(C) ;
            wait until bytes_started = bs0 + 16 ;
            t15 := start_clk ;
            wait_clk(t15 + FRAME_CLOCKS + d - 2) ;
            fire(C) ;
            wait_bytes(base + 32, "A,B chain at offset " & integer'image(d)) ;
            tick(16 * (FRAME_CLOCKS + 40) + 4 * FRAME_CLOCKS) ;
            expect_frame(base, A, "chain A at offset " & integer'image(d)) ;
            expect_frame(base + 16, B, "chain B at offset " & integer'image(d)) ;
            read_status(s) ;
            if rx_count = base + 48 then
                expect_frame(base + 32, C,
                             "chain C at offset " & integer'image(d)) ;
                assert s(3) = '0'
                    report "chain C delivered but overflow latched at offset "
                           & integer'image(d)
                    severity failure ;
                chain_accepted := chain_accepted + 1 ;
                seen_accept := true ;
            else
                assert rx_count = base + 32
                    report "chain: partial or duplicate frame at offset "
                           & integer'image(d) & ", " & integer'image(rx_count - base)
                           & " bytes"
                    severity failure ;
                assert s(3) = '1'
                    report "chain C dropped without the sticky overflow at offset "
                           & integer'image(d)
                    severity failure ;
                assert not seen_accept
                    report "chain acceptance not monotonic at offset "
                           & integer'image(d)
                    severity failure ;
                chain_refused := chain_refused + 1 ;
            end if ;
            drain_and_reset ;
        end loop ;
        report "chain sweep: accepted=" & integer'image(chain_accepted)
               & " refused=" & integer'image(chain_refused) ;
        assert chain_accepted >= 1 and chain_refused >= 1
            report "chain sweep did not cover both sides of the pending boundary"
            severity failure ;

        report "command_uart_final_stop_tb: PASS" severity note ;
        stop ;
    end process ;

end architecture ;
