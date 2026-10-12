library ieee ;
    use ieee.std_logic_1164.all ;
    use ieee.numeric_std.all ;
    use std.env.all;
    use work.time_tamer_timestamp_p.all;

entity time_tamer_tb is
end entity ;

architecture arch of time_tamer_tb is

    signal clock        :   std_logic       := '1' ;
    signal reset        :   std_logic       := '1' ;

    signal addr         :   std_logic_vector(4 downto 0) := (others => '0');
    signal din          :   std_logic_vector(7 downto 0) ;
    signal dout         :   std_logic_vector(7 downto 0) ;
    signal write        :   std_logic                       := '0' ;
    signal read         :   std_logic                       := '0' ;
    signal waitreq      :   std_logic ;
    signal readack      :   std_logic ;
    signal intr         :   std_logic ;

    signal ts_clock     :   std_logic;
    signal ts_clock_osc :   std_logic       := '1' ;
    signal ts_clock_enable : std_logic := '1';
    signal ts_reset     :   std_logic       := '1' ;
    signal ts_reset_hold : std_logic := '1';
    signal ts_time      :   std_logic_vector(63 downto 0) ;

    procedure read_time(
        signal clock    :   in  std_logic ;
        signal addr     :   out std_logic_vector(4 downto 0) ;
        signal dout     :   in std_logic_vector(7 downto 0) ;
        signal read     :   out std_logic ;
        signal readack  :   in  std_logic ;
               ts       :   out unsigned(63 downto 0)
    ) is
    begin
        for i in 0 to 7 loop
            addr <= std_logic_vector(to_unsigned(i, addr'length)) ;
            read <= '1' ;
            wait until rising_edge(clock) and readack = '1' for 20 us;
            assert readack = '1'
                report "timestamp read byte " & integer'image(i) &
                       " was not acknowledged"
                severity failure;
            report "timestamp read byte " & integer'image(i) & " acknowledged";
            ts(8*i+7 downto 8*i) := unsigned(dout) ;
            read <= '0' ;
            wait until rising_edge(clock) ;
            wait until rising_edge(clock) ;
        end loop ;
    end procedure ;

    procedure write_time(
        signal clock    :   in  std_logic ;
        signal addr     :   out std_logic_vector(4 downto 0) ;
        signal din      :   out std_logic_vector(7 downto 0) ;
        signal write    :   out std_logic ;
               ts       :   unsigned(63 downto 0)
    ) is
    begin
        for i in 0 to 7 loop
            addr <= std_logic_vector(to_unsigned(i, addr'length)) ;
            din <= std_logic_vector(ts(i*8+7 downto i*8)) ;
            write <= '1' ;
            wait until rising_edge(clock) ;
            write <= '0' ;
            wait until rising_edge(clock) ;
        end loop ;
    end procedure ;

    procedure nop( signal clock : in std_logic ; count : natural ) is
    begin
        for i in 1 to count loop
            wait until rising_edge(clock) ;
        end loop ;
    end procedure ;

    procedure clear_intr(
        signal clock    :   in  std_logic ;
        signal addr     :   out std_logic_vector(4 downto 0) ;
        signal din      :   out std_logic_vector(7 downto 0) ;
        signal write    :   out std_logic
    ) is
    begin
        addr <= std_logic_vector(to_unsigned(8, addr'length)) ;
        din <= std_logic_vector(to_unsigned(1, din'length)) ;
        write <= '1' ;
        wait until rising_edge(clock) ;
        write <= '0' ;
        -- Let the interrupt FSM finish the compare-clear/CDC acknowledgement
        -- before the caller arms another target. A two-cycle gap can drop the
        -- next command while WAIT_FOR_COMPARE_ACK_LOW is still active.
        for i in 1 to 200 loop
            wait until rising_edge(clock) ;
        end loop;
    end procedure ;

    procedure set_intr(
        signal clock    :   in  std_logic ;
        signal addr     :   out std_logic_vector(4 downto 0) ;
        signal din      :   out std_logic_vector(7 downto 0) ;
        signal write    :   out std_logic
    ) is
    begin
        addr <= std_logic_vector(to_unsigned(8, addr'length)) ;
        din <= std_logic_vector(to_unsigned(0, din'length)) ;
        write <= '1' ;
        wait until rising_edge(clock) ;
        write <= '0' ;
        wait until rising_edge(clock) ;
    end procedure ;

    procedure cancel_compare(
        signal clock    :   in  std_logic ;
        signal addr     :   out std_logic_vector(4 downto 0) ;
        signal din      :   out std_logic_vector(7 downto 0) ;
        signal write    :   out std_logic
    ) is
    begin
        addr <= std_logic_vector(to_unsigned(8, addr'length)) ;
        din <= std_logic_vector(to_unsigned(4, din'length)) ;
        write <= '1' ;
        wait until rising_edge(clock) ;
        write <= '0' ;
        wait until rising_edge(clock) ;
    end procedure ;

    procedure read_status(
        signal clock    : in  std_logic;
        signal addr     : out std_logic_vector(4 downto 0);
        signal dout     : in  std_logic_vector(7 downto 0);
        signal read     : out std_logic;
        signal readack  : in  std_logic;
        variable value  : out std_logic_vector(7 downto 0)
    ) is
    begin
        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "time-tamer status read was not acknowledged"
            severity failure;
        value := dout;
        read <= '0';
        wait until rising_edge(clock);
    end procedure;

begin

    clock <= not clock after 1 ns ;

    ts_clock_osc <= not ts_clock_osc after 12 ns ;
    ts_clock <= ts_clock_osc when ts_clock_enable = '1' else '0';

    ts_reset <= ts_reset_hold;

    U_tamer : entity work.time_tamer
      generic map (
        CANCEL_TIMEOUT_CYCLES => 63
      )
      port map (
        clock           =>  clock,
        reset           =>  reset,

        addr            =>  addr,
        din             =>  din,
        dout            =>  dout,
        write           =>  write,
        read            =>  read,
        waitreq         =>  waitreq,
        readack         =>  readack,
        intr            =>  intr,

        ts_sync_in      =>  '0',
        ts_sync_out     =>  open,

        ts_pps          =>  '0',
        ts_clock        =>  ts_clock,
        ts_reset        =>  ts_reset,
        ts_time         =>  ts_time
      ) ;

    tb : process
        variable ts : unsigned(63 downto 0) := (others =>'0') ;
        variable status_before : std_logic_vector(7 downto 0);
        variable status_after : std_logic_vector(7 downto 0);
    begin
        assert not timestamp_has_reached(
            unsigned'(x"FFFFFFFFFFFFFFFE"), unsigned'(x"0000000000000002"))
            report "future target across wrap was classified as reached"
            severity failure;
        assert timestamp_has_reached(
            unsigned'(x"0000000000000002"), unsigned'(x"FFFFFFFFFFFFFFFE"))
            report "target across wrap was not classified as reached"
            severity failure;
        assert timestamp_has_reached(
            unsigned'(x"0000000000000200"), unsigned'(x"0000000000000100"))
            report "ordinary past target was not classified as reached"
            severity failure;
        assert not timestamp_has_reached(
            unsigned'(x"0000000000000100"), unsigned'(x"0000000000000200"))
            report "ordinary future target was classified as reached"
            severity failure;
        assert not timestamp_has_reached(
            unsigned'(x"8000000000000000"), unsigned'(x"0000000000000000"))
            report "ambiguous half-range target was accepted"
            severity failure;

        reset <= '1' ;
        nop( clock, 100 ) ;

        reset <= '0' ;
        nop( clock, 100 ) ;

        -- With META disabled the timestamp domain remains in reset. Nios
        -- still clears both scheduled-retune queues during device open, so
        -- the time-tamer must acknowledge cancellation without waiting for
        -- a timestamp-domain handshake that cannot run. Its stopped heartbeat
        -- is intentional and must not be reported as a clock fault.
        nop(clock, 5000);
        read_status(clock, addr, dout, read, readack, status_before);
        assert status_before(5) = '0'
            report "timestamp reset was misreported as a stalled clock"
            severity failure;
        read_status(clock, addr, dout, read, readack, status_before);
        cancel_compare(clock, addr, din, write);
        nop(clock, 20);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(1) /= status_before(1)
            report "cancel while timestamp domain reset was not acknowledged"
            severity failure;
        status_before := status_after;
        cancel_compare(clock, addr, din, write);
        nop(clock, 20);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(1) /= status_before(1)
            report "repeated cancel while timestamp domain reset was not acknowledged"
            severity failure;

        ts_reset_hold <= '0';
        nop(clock, 100);

        nop( clock, 1000 ) ;
        read_time( clock, addr, dout, read, readack, ts ) ;

        -- Write a comparison time in the future
        write_time( clock, addr, din, write, x"0000_0000_0000_0400" ) ;
        set_intr( clock, addr, din, write ) ;

        -- The sticky system-domain arm acknowledgement is the evidence Nios
        -- needs before it reports an asynchronous compare as accepted.
        nop( clock, 100 ) ;
        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "arm-status read was not acknowledged" severity failure;
        assert dout(3) = '1'
            report "time-tamer did not publish comparator arm acknowledgement"
            severity failure;
        assert dout(6) = '0'
            report "healthy comparator arm was reported as a failed handshake"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);

        -- Wait for an interrupt. The compare that decides future-vs-past is
        -- registered a cycle ahead of the FSM branch, so this is the check
        -- that the extra cycle did not lose or duplicate the trigger: the
        -- interrupt must arrive, and it must arrive at or after the armed
        -- time, never before it.
        assert intr = '0'
            report "interrupt asserted before the armed time"
            severity failure ;
        wait until rising_edge(clock) and intr = '1' for 100 us;
        assert intr = '1' report "future compare did not raise interrupt" severity failure;
        read_time( clock, addr, dout, read, readack, ts ) ;
        assert ts >= x"0000_0000_0000_0400"
            report "interrupt fired early: timestamp " & to_hstring(ts) &
                   " is below the armed 0x400"
            severity failure ;
        report "future compare: fired at timestamp " & to_hstring(ts) ;
        nop( clock, 1000 ) ;

        -- Comparator completion is retained independently of IRQ clear so
        -- Nios can recover if delivery of this interrupt is lost.
        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "compare-status read was not acknowledged" severity failure;
        assert dout(4) = '1'
            report "time-tamer did not retain the compare-fired event"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);

        -- Clear the interrupt
        clear_intr( clock, addr, din, write ) ;

        -- The accepted compare payload must not change when software writes
        -- the next target while this request is crossing into ts_clock.
        -- Arm 0x500, then immediately change hold_time to 0x600. This request
        -- must still fire at 0x500, proving that the CDC payload was captured
        -- at command acceptance rather than at the later destination edge.
        write_time( clock, addr, din, write, x"0000_0000_0000_0500" ) ;
        set_intr( clock, addr, din, write ) ;
        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "new-arm status read was not acknowledged" severity failure;
        assert dout(4) = '0'
            report "new compare arm retained the previous fired event"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);
        write_time( clock, addr, din, write, x"0000_0000_0000_0600" ) ;
        wait until rising_edge(clock) and intr = '1' for 100 us;
        assert intr = '1' report "immutable compare did not raise interrupt" severity failure;
        read_time( clock, addr, dout, read, readack, ts ) ;
        assert ts >= x"0000_0000_0000_0500" and
               ts <  x"0000_0000_0000_0600"
            report "accepted compare payload changed after command: timestamp " &
                   to_hstring(ts) & " is outside [0x500, 0x600)"
            severity failure ;
        report "immutable compare payload: fired at timestamp " & to_hstring(ts) ;
        nop( clock, 1000 ) ;

        clear_intr( clock, addr, din, write ) ;

        -- Write a comparison time in the past
        write_time( clock, addr, din, write, x"0000_0000_0000_0400" ) ;
        set_intr( clock, addr, din, write ) ;

        -- Wait for an interrupt. This is the other branch of the same
        -- decision: the armed time has already gone by, so the FSM must take
        -- the PAST_TIME path and still raise the interrupt rather than
        -- waiting for a timestamp that will not come round again.
        wait until rising_edge(clock) and intr = '1' for 100 us;
        assert intr = '1' report "past compare did not raise interrupt" severity failure;
        report "past compare: interrupt raised" ;
        nop( clock, 1000 ) ;

        -- Clear the interrupt
        clear_intr( clock, addr, din, write ) ;

        -- A compare equal to the captured current timestamp is already due.
        -- Treat it as past instead of waiting for the 64-bit counter to wrap.
        read_time( clock, addr, dout, read, readack, ts ) ;
        write_time( clock, addr, din, write, ts ) ;
        set_intr( clock, addr, din, write ) ;
        wait until rising_edge(clock) and intr = '1' for 100 us;
        assert intr = '1' report "equal compare did not raise interrupt" severity failure;
        report "equal compare: interrupt raised as already due";
        clear_intr( clock, addr, din, write ) ;

        -- Cancel a far-future compare, then arm a second one. The canceled
        -- comparator must not consume the next arm or generate a stale IRQ.
        write_time( clock, addr, din, write, x"0000_0000_0010_0000" ) ;
        set_intr( clock, addr, din, write ) ;
        cancel_compare( clock, addr, din, write ) ;
        nop( clock, 1000 ) ;
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(7) = '0'
            report "healthy compare cancellation reported a failure"
            severity failure;
        assert intr = '0'
            report "cancelled compare generated an interrupt"
            severity failure ;
        write_time( clock, addr, din, write, x"0000_0000_0000_0400" ) ;
        set_intr( clock, addr, din, write ) ;
        wait until rising_edge(clock) and intr = '1' for 100 us;
        assert intr = '1'
            report "time-tamer did not re-arm after compare cancellation"
            severity failure ;
        clear_intr( clock, addr, din, write ) ;

        -- A cancel CDC which receives no timestamp-domain response must
        -- fail on the independent system clock. Recovery plus a new cancel
        -- request can still obtain the real ACK and clear the diagnostic.
        write_time(clock, addr, din, write,
                   x"0000_0000_0010_0000");
        set_intr(clock, addr, din, write);
        nop(clock, 100);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(3) = '1'
            report "cancel-watchdog compare did not arm"
            severity failure;
        ts_clock_enable <= '0';
        cancel_compare(clock, addr, din, write);
        nop(clock, 100);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(7) = '1' and intr = '0'
            report "missing cancel ACK was not reported fail-closed"
            severity failure;
        ts_clock_enable <= '1';
        nop(clock, 100);
        cancel_compare(clock, addr, din, write);
        nop(clock, 100);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(7) = '0' and intr = '0'
            report "cancel recovery did not clear the failure diagnostic"
            severity failure;

        -- A one-tick lead can expire while the MM-to-ts-clock arm handshake
        -- is in flight. It must still fire promptly instead of wrapping.
        read_time( clock, addr, dout, read, readack, ts ) ;
        write_time( clock, addr, din, write, ts + 1 ) ;
        set_intr( clock, addr, din, write ) ;
        wait until rising_edge(clock) and intr = '1' for 100 us;
        assert intr = '1'
            report "near-current compare did not fire"
            severity failure ;
        clear_intr( clock, addr, din, write ) ;

        -- A stopped timestamp clock must not hold the Avalon master forever.
        -- The read completes through its watchdog and status bit 2 marks the
        -- returned snapshot invalid. Once the clock resumes, a new snapshot
        -- clears the diagnostic and returns to normal operation.
        ts_clock_enable <= '0';
        addr <= std_logic_vector(to_unsigned(0, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1'
            report "timestamp snapshot timeout held Avalon read forever"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);

        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "invalid-snapshot status read was not acknowledged" severity failure;
        assert dout(2) = '1'
            report "timestamp snapshot timeout was not reported"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);

        ts_clock_enable <= '1';
        read_time( clock, addr, dout, read, readack, ts ) ;
        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "recovered timestamp read was not acknowledged" severity failure;
        assert dout(2) = '0'
            report "valid timestamp snapshot retained stale timeout status"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);

        -- The timestamp clock may stop while the host-to-ts compare handshake
        -- is still in flight. The independent schedule watchdog must report
        -- that missing ACK as failure, never as an accepted compare.
        ts_clock_enable <= '0';
        write_time(clock, addr, din, write,
                   x"0000_0000_0010_0000");
        set_intr(clock, addr, din, write);
        nop(clock, 5000);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(3) = '0' and status_after(4) = '0' and
               status_after(5) = '1' and status_after(6) = '1'
            report "missing compare-arm ACK was not reported as failure"
            severity failure;
        ts_clock_enable <= '1';
        nop(clock, 100);
        cancel_compare(clock, addr, din, write);
        nop(clock, 100);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(6) = '0' and status_after(7) = '0'
            report "cancel did not clear the failed-arm diagnostic"
            severity failure;

        nop( clock, 1000 ) ;

        -- Read the time back
        read_time( clock, addr, dout, read, readack, ts ) ;

        nop( clock, 1000 ) ;

        -- Once a future compare has been accepted, loss of ts_clock must be
        -- reported independently of that clock. It must not look like a
        -- successful compare or leave the Nios queue pending forever.
        write_time( clock, addr, din, write,
                    x"0000_0000_0010_0000" ) ;
        set_intr( clock, addr, din, write ) ;
        nop(clock, 100);
        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "later timestamp status read was not acknowledged" severity failure;
        assert dout(3) = '1'
            report "running timestamp clock did not acknowledge compare arm"
            severity failure;
        assert dout(6) = '0'
            report "far-future compare was reported as a schedule failure"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);
        nop(clock, 5000);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(3) = '1' and status_after(6) = '0' and intr = '0'
            report "schedule watchdog treated a far-future target as a timeout"
            severity failure;
        ts_clock_enable <= '0';
        nop(clock, 5000);
        addr <= std_logic_vector(to_unsigned(9, addr'length));
        read <= '1';
        wait until rising_edge(clock) and readack = '1' for 20 us;
        assert readack = '1' report "final status read was not acknowledged" severity failure;
        assert dout(5) = '1' and dout(4) = '0'
            report "stopped timestamp clock was not reported as a failed compare"
            severity failure;
        read <= '0';
        wait until rising_edge(clock);

        -- A recovered heartbeat clears the sticky stall status without
        -- requiring another arm/cancel command. Nios uses this transition to
        -- retry cancellation while retaining queue ownership.
        ts_clock_enable <= '1';
        nop(clock, 100);
        read_status(clock, addr, dout, read, readack, status_after);
        assert status_after(5) = '0'
            report "timestamp-clock recovery did not clear stalled status"
            severity failure;

        report "time_tamer_tb: PASS" severity note;
        stop;
        wait;
    end process ;

end architecture ;
