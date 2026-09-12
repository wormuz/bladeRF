-- Testbench for pretrigger_buffer.
--
-- The failure modes worth testing are all about WHICH samples survive:
--
--   1. the ring holds the samples BEFORE the trigger, not after -- getting
--      this backwards produces a buffer that duplicates the datapath and
--      loses the onset, which is the only reason the block exists;
--   2. it keeps recording until the trigger and stops exactly there;
--   3. a dwell boundary discards history, because history from before a
--      retune belongs to a different frequency;
--   4. wrapped distinguishes "the ring filled" from "the dwell was short",
--      so the host does not read stale zeros as signal.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity pretrigger_buffer_tb is
end entity;

architecture sim of pretrigger_buffer_tb is

    constant DEPTH_LOG2 : natural := 4;         -- 16 entries
    constant DEPTH      : natural := 2 ** DEPTH_LOG2;

    signal clock        : std_logic := '0';
    signal reset        : std_logic := '1';
    signal sample       : sample_stream_t := ZERO_SAMPLE;
    signal trigger      : std_logic := '0';
    signal dwell_start  : std_logic := '0';
    signal frozen       : std_logic;
    signal oldest_index : unsigned(DEPTH_LOG2-1 downto 0);
    signal wrapped      : std_logic;
    signal rd_addr      : unsigned(DEPTH_LOG2-1 downto 0) := (others => '0');
    signal rd_data      : std_logic_vector(31 downto 0);

    signal done         : boolean := false;

begin

    clock <= not clock after 5 ns when not done else '0';

    U_dut : entity work.pretrigger_buffer
        generic map ( DEPTH_LOG2 => DEPTH_LOG2 )
        port map (
            clock        => clock,
            reset        => reset,
            sample       => sample,
            trigger      => trigger,
            dwell_start  => dwell_start,
            frozen       => frozen,
            oldest_index => oldest_index,
            wrapped      => wrapped,
            rd_addr      => rd_addr,
            rd_data      => rd_data
        );

    stim : process

        procedure tick( n : natural := 1 ) is
        begin
            for i in 1 to n loop
                wait until rising_edge(clock);
            end loop;
        end procedure;

        -- Feed samples whose I value is the sequence number, so a read can
        -- say exactly which sample it got.
        procedure feed( first : natural; n : natural ) is
        begin
            for i in 0 to n-1 loop
                sample.data_i <= to_signed(first + i, 16);
                sample.data_q <= to_signed(0, 16);
                sample.data_v <= '1';
                wait until rising_edge(clock);
            end loop;
            sample.data_v <= '0';
            wait until rising_edge(clock);
        end procedure;

        procedure pulse( signal s : out std_logic ) is
        begin
            s <= '1';
            wait until rising_edge(clock);
            s <= '0';
            wait until rising_edge(clock);
        end procedure;

        -- Registered read: address one cycle, data the next.
        impure function read_at( a : natural ) return integer is
        begin
            return to_integer(signed(rd_data(15 downto 0)));
        end function;

        variable got : integer;

    begin
        tick(4);
        reset <= '0';
        tick(2);

        ------------------------------------------------------------------
        -- 1. Partial fill: 5 samples, no wrap.
        ------------------------------------------------------------------
        pulse(dwell_start);
        feed(100, 5);
        assert wrapped = '0'
            report "FAIL: wrapped set after only 5 of 16 samples"
            severity error;
        report "case 1 OK: 5 samples, wrapped = 0";

        ------------------------------------------------------------------
        -- 2. Fill past the end: the ring wraps and keeps the LATEST DEPTH.
        --    Feeding 100..123 (24 samples) into 16 entries must leave
        --    108..123, and the oldest must be 108.
        ------------------------------------------------------------------
        pulse(dwell_start);
        feed(100, 24);
        assert wrapped = '1'
            report "FAIL: 24 samples into a 16-entry ring did not wrap"
            severity error;

        pulse(trigger);
        assert frozen = '1'
            report "FAIL: trigger did not freeze the ring"
            severity error;

        rd_addr <= oldest_index;
        tick(2);
        got := read_at(0);
        assert got = 108
            report "FAIL: oldest entry is " & integer'image(got)
                   & ", expected 108 -- the ring is not keeping the most "
                   & "recent DEPTH samples"
            severity error;
        report "case 2 OK: wrapped, oldest = " & integer'image(got)
               & " at index " & integer'image(to_integer(oldest_index));

        ------------------------------------------------------------------
        -- 3. Frozen means frozen: samples after the trigger must not
        --    overwrite the history. This is the one that turns the block
        --    into a duplicate of the datapath if it is wrong.
        ------------------------------------------------------------------
        feed(900, 8);
        rd_addr <= oldest_index;
        tick(2);
        got := read_at(0);
        assert got = 108
            report "FAIL: post-trigger samples overwrote the history "
                   & "(oldest is now " & integer'image(got) & ")"
            severity error;
        report "case 3 OK: 8 post-trigger samples did not disturb the ring";

        ------------------------------------------------------------------
        -- 4. A dwell boundary re-arms and discards.
        ------------------------------------------------------------------
        pulse(dwell_start);
        assert frozen = '0' and wrapped = '0'
            report "FAIL: dwell boundary did not re-arm the ring"
            severity error;
        report "case 4 OK: dwell boundary re-armed and cleared wrapped";

        ------------------------------------------------------------------
        -- 5. Trigger and dwell_start in the same cycle: re-arm wins. The
        --    trigger belongs to the dwell that is ending and its history
        --    is about to be discarded, so freezing would keep a ring the
        --    host cannot attribute to any frequency.
        ------------------------------------------------------------------
        feed(200, 20);
        trigger     <= '1';
        dwell_start <= '1';
        tick(1);
        trigger     <= '0';
        dwell_start <= '0';
        tick(1);
        assert frozen = '0'
            report "FAIL: simultaneous trigger and dwell start froze the "
                   & "ring instead of re-arming"
            severity error;
        report "case 5 OK: dwell boundary wins over a same-cycle trigger";

        report "pretrigger_buffer_tb: all cases passed";
        done <= true;
        wait;
    end process;

end architecture;
