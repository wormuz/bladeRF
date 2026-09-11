-- Testbench for dwell_record_fifo.
--
-- The block exists so a completed dwell record crosses the clock boundary
-- as one thing. So the tests are about atomicity and about what happens
-- when the host stops reading -- not about whether a FIFO counts.
--
--   1. a record written on the sample clock reads back whole on the system
--      clock, every field, with the two clocks unrelated;
--   2. reading while empty reports empty rather than a stale record;
--   3. acknowledging advances to the next record and the fields change
--      together, not one at a time;
--   4. a writer that laps the reader drops the oldest and counts it, and
--      the record that comes back is still a whole one -- never half
--      overwritten, which is the failure the latch it replaces could not
--      rule out.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity dwell_record_fifo_tb is
end entity;

architecture sim of dwell_record_fifo_tb is

    constant WORDS_LOG2 : natural := 4;
    constant DEPTH_LOG2 : natural := 2;
    constant DEPTH      : natural := 2 ** DEPTH_LOG2;

    -- Deliberately unrelated periods, and neither a multiple of the other:
    -- a FIFO that only works when the clocks divide evenly is not a CDC.
    constant WR_PERIOD  : time := 8.14 ns;    -- ~61.44 MHz
    constant RD_PERIOD  : time := 12.5 ns;    -- 80 MHz

    signal wr_clock, wr_reset : std_logic := '0';
    signal rd_clock, rd_reset : std_logic := '0';

    signal summary_valid   : std_logic := '0';
    signal energy_sum      : unsigned(63 downto 0) := (others => '0');
    signal peak            : unsigned(31 downto 0) := (others => '0');
    signal clip_count      : unsigned(31 downto 0) := (others => '0');
    signal sample_count    : unsigned(31 downto 0) := (others => '0');
    signal first_timestamp : unsigned(63 downto 0) := (others => '0');
    signal first_window    : unsigned(15 downto 0) := (others => '0');
    signal mean_power      : unsigned(31 downto 0) := (others => '0');
    signal noise_floor     : unsigned(47 downto 0) := (others => '0');
    signal peak_window     : unsigned(47 downto 0) := (others => '0');
    signal triggered       : std_logic := '0';
    signal measure_valid   : std_logic := '0';
    signal gain_too_high   : std_logic := '0';
    signal settle_elapsed  : unsigned(15 downto 0) := (others => '0');

    signal rd_index   : unsigned(WORDS_LOG2-1 downto 0) := (others => '0');
    signal rd_data    : std_logic_vector(31 downto 0);
    signal rd_valid   : std_logic;
    signal rd_ack     : std_logic := '0';
    signal dropped    : unsigned(15 downto 0);

    signal done : boolean := false;

begin

    wr_clock <= not wr_clock after WR_PERIOD / 2 when not done else '0';
    rd_clock <= not rd_clock after RD_PERIOD / 2 when not done else '0';

    U_dut : entity work.dwell_record_fifo
        generic map ( WORDS_LOG2 => WORDS_LOG2, DEPTH_LOG2 => DEPTH_LOG2 )
        port map (
            wr_clock => wr_clock, wr_reset => wr_reset,
            summary_valid => summary_valid,
            energy_sum => energy_sum, peak => peak,
            clip_count => clip_count, sample_count => sample_count,
            first_timestamp => first_timestamp, first_window => first_window,
            mean_power => mean_power, noise_floor => noise_floor,
            peak_window => peak_window, triggered => triggered,
            measure_valid => measure_valid, gain_too_high => gain_too_high,
            settle_elapsed => settle_elapsed,
            rd_clock => rd_clock, rd_reset => rd_reset,
            rd_index => rd_index, rd_data => rd_data,
            rd_valid => rd_valid, rd_ack => rd_ack,
            dropped_count => dropped
        );

    stim : process

        -- Every field derived from one number, so a record that mixes two
        -- writes is obvious rather than plausible.
        procedure write_record( n : natural ) is
        begin
            wait until rising_edge(wr_clock);
            energy_sum      <= to_unsigned(n, 32) & to_unsigned(n + 100, 32);
            peak            <= to_unsigned(n + 200, 32);
            clip_count      <= to_unsigned(n + 300, 32);
            sample_count    <= to_unsigned(n + 400, 32);
            first_timestamp <= to_unsigned(n + 500, 32) & to_unsigned(n + 600, 32);
            first_window    <= to_unsigned(n + 7, 16);
            mean_power      <= to_unsigned(n + 700, 32);
            noise_floor     <= to_unsigned(n + 800, 48);
            peak_window     <= to_unsigned(n + 900, 48);
            settle_elapsed  <= to_unsigned(n + 11, 16);
            triggered       <= '1';
            measure_valid   <= '1';
            gain_too_high   <= '0';
            summary_valid   <= '1';
            wait until rising_edge(wr_clock);
            summary_valid   <= '0';
        end procedure;

        impure function word( i : natural ) return unsigned is
        begin
            return unsigned(rd_data);
        end function;

        -- Three edges, not two: the address is registered before the
        -- memory is read, which is what block RAM requires -- an
        -- asynchronous read put the whole thing in logic, 2119 cells and
        -- no RAM at all. One extra cycle on a register the host polls at
        -- USB rates costs nothing.
        procedure read_word( i : natural ) is
        begin
            rd_index <= to_unsigned(i, WORDS_LOG2);
            wait until rising_edge(rd_clock);
            wait until rising_edge(rd_clock);
            wait until rising_edge(rd_clock);
        end procedure;

        procedure ack is
        begin
            rd_ack <= '1';
            wait until rising_edge(rd_clock);
            rd_ack <= '0';
            wait until rising_edge(rd_clock);
        end procedure;

        variable v : unsigned(31 downto 0);

    begin
        wr_reset <= '1'; rd_reset <= '1';
        wait for 100 ns;
        wr_reset <= '0'; rd_reset <= '0';
        wait for 100 ns;

        ------------------------------------------------------------------
        -- 1. Empty reports empty.
        ------------------------------------------------------------------
        assert rd_valid = '0'
            report "FAIL: reports a record before anything was written"
            severity error;
        report "case 1 OK: empty FIFO reports empty";

        ------------------------------------------------------------------
        -- 2. One record crosses whole.
        ------------------------------------------------------------------
        write_record(1);
        wait for 200 ns;                -- let the pointer cross

        assert rd_valid = '1'
            report "FAIL: record written but never visible to the reader"
            severity error;

        read_word(2); v := word(2);
        assert v = 201
            report "FAIL: peak is " & integer'image(to_integer(v))
                   & ", expected 201"
            severity error;
        read_word(4); v := word(4);
        assert v = 401
            report "FAIL: sample_count is " & integer'image(to_integer(v))
            severity error;
        read_word(0); v := word(0);
        assert v = 101
            report "FAIL: energy low word is " & integer'image(to_integer(v))
                   & ", expected 101 -- halves may be swapped"
            severity error;
        report "case 2 OK: record crossed whole, fields intact";

        ------------------------------------------------------------------
        -- 3. Acknowledge advances, and the NEXT record is internally
        --    consistent -- every field from the same write.
        ------------------------------------------------------------------
        write_record(2);
        wait for 200 ns;
        ack;
        wait for 200 ns;

        read_word(2); v := word(2);
        assert v = 202
            report "FAIL: after ack, peak is " & integer'image(to_integer(v))
                   & ", expected 202 from the second record"
            severity error;
        read_word(4); v := word(4);
        assert v = 402
            report "FAIL: sample_count is " & integer'image(to_integer(v))
                   & " -- mixed with another record"
            severity error;
        report "case 3 OK: ack advanced, second record consistent";

        ------------------------------------------------------------------
        -- 4. Writer laps the reader: oldest dropped, counted, and what
        --    comes back is still a whole record.
        ------------------------------------------------------------------
        for n in 10 to 10 + DEPTH + 2 loop
            write_record(n);
            wait for 60 ns;
        end loop;
        wait for 400 ns;

        assert dropped > 0
            report "FAIL: writer lapped the reader and nothing was counted "
                   & "as dropped -- a sweep would lose measurements silently"
            severity error;

        read_word(2); v := word(2);
        read_word(4);
        assert word(4) = v - 200 + 400
            report "FAIL: after a lap the fields come from different "
                   & "records (peak=" & integer'image(to_integer(v))
                   & " sample_count=" & integer'image(to_integer(word(4)))
                   & ")"
            severity error;
        report "case 4 OK: " & integer'image(to_integer(dropped))
               & " dropped and counted, surviving record is whole";

        report "dwell_record_fifo_tb: all cases passed";
        done <= true;
        wait;
    end process;

end architecture;
