library ieee ;
    use ieee.std_logic_1164.all ;
    use ieee.numeric_std.all ;

library std ;
    use std.textio.all ;
    use std.env.all;

entity command_uart_tb is
end entity ;

architecture arch of command_uart_tb is

    signal clock        :   std_logic       := '1' ;
    signal reset        :   std_logic       := '1' ;
    signal host_session_reset : std_logic := '0';
    signal dev_session_reset  : std_logic := '0';
    signal host_session_generation : std_logic_vector(31 downto 0) := (others => '0');
    signal dev_session_generation  : std_logic_vector(31 downto 0) := (others => '0');
    signal stale_response_attempted : std_logic := '0';
    signal device_reconnected : std_logic := '0';
    signal response_reset_test_done : std_logic := '0';

    signal host_sin     :   std_logic ;
    signal host_sout    :   std_logic ;

    signal host_addr    :   std_logic_vector(4 downto 0) ;
    signal host_dout    :   std_logic_vector(31 downto 0) ;
    signal host_din     :   std_logic_vector(31 downto 0) ;
    signal host_read    :   std_logic                       := '0' ;
    signal host_write   :   std_logic                       := '0' ;
    signal host_readack :   std_logic ;
    signal host_waitreq :   std_logic ;
    signal host_irq     :   std_logic ;

    signal dev_addr     :   std_logic_vector(4 downto 0) ;
    signal dev_dout     :   std_logic_vector(31 downto 0) ;
    signal dev_din      :   std_logic_vector(31 downto 0) ;
    signal dev_read     :   std_logic                        := '0' ;
    signal dev_write    :   std_logic                        := '0' ;
    signal dev_readack  :   std_logic ;
    signal dev_waitreq  :   std_logic ;
    signal dev_irq      :   std_logic ;

    procedure nop( signal clock : in std_logic ; count : in natural ) is
    begin
        for i in 1 to count loop
            wait until rising_edge(clock) ;
        end loop ;
    end procedure ;

    procedure write_command(
        signal clock : in std_logic ;
        signal addr : out std_logic_vector(4 downto 0) ;
        signal din : out std_logic_vector(31 downto 0) ;
        signal write : out std_logic ;
               command : in std_logic_vector(127 downto 0)
    ) is
    begin
        for i in 0 to 3 loop
            addr <= std_logic_vector(to_unsigned(4*i, addr'length)) ;
            din <= command(i*32+31 downto i*32) ;
            write <= '1' ;
            wait until rising_edge(clock) ;
            write <= '0' ;
            wait until rising_edge(clock) ;
        end loop ;
    end procedure ;

    procedure write_word(
        signal clock : in std_logic ;
        signal addr : out std_logic_vector(4 downto 0) ;
        signal din : out std_logic_vector(31 downto 0) ;
        signal write : out std_logic ;
               offset : in natural ;
               value : in std_logic_vector(31 downto 0)
    ) is
    begin
        addr <= std_logic_vector(to_unsigned(offset, addr'length));
        din <= value;
        write <= '1';
        wait until rising_edge(clock);
        write <= '0';
        wait until rising_edge(clock);
    end procedure;

    procedure commit_response(
        signal clock : in std_logic;
        signal addr : out std_logic_vector(4 downto 0);
        signal din : out std_logic_vector(31 downto 0);
        signal write : out std_logic;
        constant word0 : in std_logic_vector(31 downto 0);
        constant word1 : in std_logic_vector(31 downto 0);
        constant word2 : in std_logic_vector(31 downto 0);
        constant word3 : in std_logic_vector(31 downto 0)
    ) is
    begin
        write_word(clock, addr, din, write, 24, x"00000001");
        write_word(clock, addr, din, write, 0, word0);
        write_word(clock, addr, din, write, 4, word1);
        write_word(clock, addr, din, write, 8, word2);
        write_word(clock, addr, din, write, 12, word3);
    end procedure;

    procedure read_command(
        signal clock : in std_logic ;
        signal addr : out std_logic_vector(4 downto 0) ;
        signal dout : in std_logic_vector(31 downto 0) ;
        signal read : out std_logic ;
               command : out std_logic_vector(127 downto 0)
    ) is
    begin
        for i in 0 to 3 loop
            addr <= std_logic_vector(to_unsigned(4*i, addr'length)) ;
            read <= '1' ;
            wait until rising_edge(clock) ;
            read <= '1' ;
            wait until rising_edge(clock) ;
            read <= '0' ;
            command(i*32+31 downto i*32) := dout ;
            nop( clock, 10 ) ;
        end loop ;
    end procedure ;

    procedure send_retune(
        signal clock        :   in      std_logic ;
        signal addr         :   out     std_logic_vector(4 downto 0) ;
        signal din          :   out     std_logic_vector(31 downto 0) ;
        signal write        :   out     std_logic ;
               timestamp    :   in      std_logic_vector(63 downto 0) ;
               nint         :   in      positive ;
               nfrac        :   in      natural ;
               freqsel      :   in      natural range 0 to 63 ;
               vcocap       :   in      natural range 0 to 63 ;
               tx           :   in      std_logic ;
               rx           :   in      std_logic ;
               bandsel      :   in      std_logic
    ) is
        variable data : std_logic_vector(127 downto 0) ;
        constant nint_slv : std_logic_vector(8 downto 0) := std_logic_vector(to_unsigned(nint,9)) ;
        constant nfrac_slv : std_logic_vector(22 downto 0) := std_logic_vector(to_unsigned(nfrac,23)) ;
        constant freqsel_slv : std_logic_vector(5 downto 0) := std_logic_vector(to_unsigned(freqsel,6)) ;
        constant vcocap_slv : std_logic_vector(5 downto 0) := std_logic_vector(to_unsigned(vcocap,6)) ;
    begin
        data( 0*8+7 downto  0*8)    := std_logic_vector(to_unsigned(character'pos('T'),8)) ;
        data( 8*8+7 downto  1*8)    := timestamp ;
        data(12*8+7 downto  9*8)    := nfrac_slv(7 downto 0) & nfrac_slv(15 downto 8) & nint_slv(0) & nfrac_slv(22 downto 16) & nint_slv(8 downto 1) ;
        data(13*8+7 downto 13*8)    := tx & rx & freqsel_slv ;
        data(14*8+7 downto 14*8)    := bandsel & '0' & vcocap_slv ;
        data(15*8+7 downto 15*8)    := (others =>'0') ;
        write_command(clock, addr, din, write, data) ;
    end procedure ;

begin

    clock <= not clock after 1 ns ;

    U_host : entity work.command_uart
      port map (
        clock       =>  clock,
        reset       =>  reset,

        rs232_sin   =>  host_sin,
        rs232_sout  =>  host_sout,
        session_reset => host_session_reset,
        session_generation => host_session_generation,

        addr        =>  host_addr,
        din         =>  host_din,
        dout        =>  host_dout,
        read        =>  host_read,
        write       =>  host_write,
        readack     =>  host_readack,
        waitreq     =>  host_waitreq,
        irq         =>  host_irq
      ) ;

    U_device : entity work.command_uart
      port map (
        clock       =>  clock,
        reset       =>  reset,

        rs232_sin   =>  host_sout,
        rs232_sout  =>  host_sin,
        session_reset => dev_session_reset,
        session_generation => dev_session_generation,

        addr        =>  dev_addr,
        din         =>  dev_din,
        dout        =>  dev_dout,
        read        =>  dev_read,
        write       =>  dev_write,
        readack     =>  dev_readack,
        waitreq     =>  dev_waitreq,
        irq         =>  dev_irq
      ) ;

    tb : process
        variable response : std_logic_vector(127 downto 0) ;
    begin
        nop( clock, 100 ) ;
        reset <= '0' ;
        nop( clock, 100 ) ;

        -- Send a retune command for giggles
        report "HOST: Send retune command" ;
        send_retune(
            clock       => clock,
            addr        => host_addr,
            din         => host_din,
            write       => host_write,
            timestamp   => (others =>'0'),
            nint        => 131,
            nfrac       => 0,
            vcocap      => 32,
            freqsel     => 44,
            tx          => '0',
            rx          => '0',
            bandsel     => '0'
        ) ;

        -- Let the first request enter NIOS, then send a second request while
        -- that handler is still busy across the simulated reconnect.
        wait until device_reconnected = '1';

        -- A fresh request in the new session must be held until NIOS finishes.
        write_word(clock, host_addr, host_din, host_write, 24,
                   x"00000001");
        send_retune(
            clock       => clock,
            addr        => host_addr,
            din         => host_din,
            write       => host_write,
            timestamp   => (others => '0'),
            nint        => 131,
            nfrac       => 0,
            vcocap      => 32,
            freqsel     => 44,
            tx          => '0',
            rx          => '0',
            bandsel     => '0'
        );
        wait until stale_response_attempted = '1';
        assert host_irq = '0'
            report "stale response crossed an FX3 session reset" severity failure;
        wait until rising_edge(clock) and host_irq = '1';
        nop(clock, 500);
        report "HOST: Received response in new session";
        read_command(clock, host_addr, host_dout, host_read, response);

        nop( clock, 1000 ) ;

        -- The device process queues two test responses behind the real NIOS
        -- reply. They must remain immutable and arrive in order.
        wait until rising_edge(clock) and host_irq = '1';
        read_command(clock, host_addr, host_dout, host_read, response);
        assert response = x"A1A1A1A1A1A1A1A1A1A1A1A1A1A1A143"
            report "first committed response was modified in flight"
            severity failure;

        wait until rising_edge(clock) and host_irq = '1';
        read_command(clock, host_addr, host_dout, host_read, response);
        assert response = x"B2B2B2B2B2B2B2B2B2B2B2B2B2B2B243"
            report "pending response was lost or reordered"
            severity failure;

        wait until response_reset_test_done = '1';
        nop(clock, 4000);
        assert host_irq = '0'
            report "queued pre-reset response escaped into the new session"
            severity failure;

        report "command_uart_tb: PASS" severity note;
        stop;

    end process ;

    -- Simulate the NIOS command handler. On its first request, force an FX3
    -- reconnect before it writes the response, then attempt the stale reply.
    receive_command : process
        variable request : std_logic_vector(127 downto 0);
        variable request_generation : std_logic_vector(31 downto 0);
        variable first_request : boolean := true;
        variable ordinary_response_started : boolean := false;
    begin
        -- Hold the ISR disabled while the first frame arrives. Once it is
        -- complete, assert worker_busy while it is pending, clear busy, then
        -- release the ISR. The request's latched busy state must survive.
        write_word(clock, dev_addr, dev_din, dev_write, 16, x"00000000");
        loop
            if dev_irq /= '1' then
                if first_request then
                    nop(clock, 4500);
                    write_word(clock, dev_addr, dev_din, dev_write, 16,
                               x"00000002");
                    nop(clock, 10);
                    write_word(clock, dev_addr, dev_din, dev_write, 16,
                               x"00000001");
                else
                    wait until rising_edge(clock) and dev_irq = '1';
                end if;
            end if;
            report "DEVICE: Received request";
            nop(clock, 100);
            if first_request then
                dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                dev_read <= '1';
                wait until rising_edge(clock);
                wait for 1 ns;
                assert dev_dout(0) = '1'
                    report "request lost worker-busy state before delayed ISR"
                    severity failure;
                dev_read <= '0';
            else
                dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                dev_read <= '1';
                wait until rising_edge(clock);
                wait for 1 ns;
                assert dev_dout(0) = '0'
                    report "busy evidence leaked from the previous request"
                    severity failure;
                dev_read <= '0';
            end if;
            read_command(clock, dev_addr, dev_dout, dev_read, request);
            if first_request then
                dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                dev_read <= '1';
                wait until rising_edge(clock);
                wait for 1 ns;
                assert dev_dout(0) = '0'
                    report "request-busy latch did not clear when request was consumed"
                    severity failure;
                dev_read <= '0';
            end if;
            dev_addr <= std_logic_vector(to_unsigned(20, dev_addr'length));
            dev_read <= '1';
            wait until rising_edge(clock);
            wait for 1 ns;
            request_generation := dev_dout;
            dev_read <= '0';

            if first_request then
                first_request := false;
                write_word(clock, dev_addr, dev_din, dev_write, 16,
                           x"00000000");
                dev_session_generation <= x"00000001";
                host_session_generation <= x"00000001";
                dev_session_reset <= '1';
                host_session_reset <= '1';
                nop(clock, 4);
                dev_session_reset <= '0';
                host_session_reset <= '0';
                device_reconnected <= '1';
                dev_addr <= std_logic_vector(to_unsigned(24, dev_addr'length));
                dev_read <= '1';
                wait until rising_edge(clock);
                wait for 1 ns;
                assert dev_dout = x"00000001"
                    report "current command UART session was not readable"
                    severity failure;
                dev_read <= '0';
                nop(clock, 10000);
            end if;

            write_word(clock, dev_addr, dev_din, dev_write, 24, request_generation);
            report "DEVICE: Writing response";
            write_command(clock, dev_addr, dev_din, dev_write, request);
            if not first_request then
                if request_generation = x"00000000" then
                    stale_response_attempted <= '1';
                    nop(clock, 50);
                    write_word(clock, dev_addr, dev_din, dev_write, 16,
                               x"00000001");
                end if;
            end if;
            if request_generation = x"00000001" then
                -- Let the ordinary reply drain, then create a deterministic
                -- overlap: response B commits while A is on the UART wire.
                loop
                    dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                    dev_read <= '1';
                    wait until rising_edge(clock);
                    wait for 1 ns;
                    if dev_dout(4) = '1' then
                        ordinary_response_started := true;
                    end if;
                    exit when ordinary_response_started and
                              dev_dout(4) = '0' and dev_dout(2) = '0';
                    dev_read <= '0';
                    wait until rising_edge(clock);
                end loop;
                dev_read <= '0';
                commit_response(clock, dev_addr, dev_din, dev_write,
                                x"A1A1A143", x"A1A1A1A1",
                                x"A1A1A1A1", x"A1A1A1A1");
                commit_response(clock, dev_addr, dev_din, dev_write,
                                x"B2B2B243", x"B2B2B2B2",
                                x"B2B2B2B2", x"B2B2B2B2");

                dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                dev_read <= '1';
                wait until rising_edge(clock);
                wait for 1 ns;
                assert dev_dout(4) = '1' and dev_dout(2) = '1' and
                       dev_dout(3) = '0'
                    report "response pending/full status is incorrect"
                    severity failure;
                dev_read <= '0';

                -- Wait until A/B leave the serializer, then force the
                -- active+pending limit, verify sticky overflow, and reset
                -- the session while both accepted frames are still queued.
                loop
                    dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                    dev_read <= '1';
                    wait until rising_edge(clock);
                    wait for 1 ns;
                    exit when dev_dout(4) = '0' and dev_dout(2) = '0';
                    dev_read <= '0';
                    wait until rising_edge(clock);
                end loop;
                dev_read <= '0';
                commit_response(clock, dev_addr, dev_din, dev_write,
                                x"C3C3C343", x"C3C3C3C3",
                                x"C3C3C3C3", x"C3C3C3C3");
                commit_response(clock, dev_addr, dev_din, dev_write,
                                x"D4D4D443", x"D4D4D4D4",
                                x"D4D4D4D4", x"D4D4D4D4");
                commit_response(clock, dev_addr, dev_din, dev_write,
                                x"E5E5E543", x"E5E5E5E5",
                                x"E5E5E5E5", x"E5E5E5E5");

                dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                dev_read <= '1';
                wait until rising_edge(clock);
                wait for 1 ns;
                assert dev_dout(4) = '1' and dev_dout(2) = '1' and
                       dev_dout(3) = '1'
                    report "third response did not latch overflow while failing closed"
                    severity failure;
                dev_read <= '0';

                dev_session_generation <= x"00000002";
                host_session_generation <= x"00000002";
                dev_session_reset <= '1';
                host_session_reset <= '1';
                nop(clock, 4);
                dev_session_reset <= '0';
                host_session_reset <= '0';
                -- The helper carries the old generation; a stale commit must
                -- not restart the just-flushed serializer.
                commit_response(clock, dev_addr, dev_din, dev_write,
                                x"F6F6F643", x"F6F6F6F6",
                                x"F6F6F6F6", x"F6F6F6F6");
                nop(clock, 4);
                dev_addr <= std_logic_vector(to_unsigned(28, dev_addr'length));
                dev_read <= '1';
                wait until rising_edge(clock);
                wait for 1 ns;
                assert dev_dout(4) = '0' and dev_dout(2) = '0' and
                       dev_dout(3) = '0'
                    report "stale-session response restarted the serializer"
                    severity failure;
                dev_read <= '0';
                response_reset_test_done <= '1';
            end if;
        end loop;
    end process;

end architecture ;
