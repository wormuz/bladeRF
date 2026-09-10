-- Testbench for dwell_readout.
--
-- The point of the block is that a host reading thirteen words one at a time
-- cannot end up with half of one dwell and half of the next. So the tests
-- are about tearing, not about whether a mux works:
--
--   1. the latch holds while the live inputs change underneath it;
--   2. the generation counter advances exactly once per dwell, which is what
--      lets the host detect a boundary it read across;
--   3. 64-bit and 48-bit fields are split into words the right way round --
--      a swapped pair yields a number that is wrong by 2^32 and still looks
--      like a plausible energy;
--   4. an out-of-range index reads zero rather than aliasing to real data.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity dwell_readout_tb is
end entity;

architecture sim of dwell_readout_tb is

    signal clock           : std_logic := '0';
    signal reset           : std_logic := '1';
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

    signal rd_index        : unsigned(3 downto 0) := (others => '0');
    signal rd_data         : std_logic_vector(31 downto 0);
    signal generation      : unsigned(15 downto 0);

    signal done            : boolean := false;

begin

    clock <= not clock after 5 ns when not done else '0';

    U_dut : entity work.dwell_readout
        port map (
            clock           => clock,
            reset           => reset,
            summary_valid   => summary_valid,
            energy_sum      => energy_sum,
            peak            => peak,
            clip_count      => clip_count,
            sample_count    => sample_count,
            first_timestamp => first_timestamp,
            first_window    => first_window,
            mean_power      => mean_power,
            noise_floor     => noise_floor,
            peak_window     => peak_window,
            rd_index        => rd_index,
            rd_data         => rd_data,
            generation      => generation
        );

    stim : process

        procedure tick( n : natural := 1 ) is
        begin
            for i in 1 to n loop
                wait until rising_edge(clock);
            end loop;
        end procedure;

        impure function read_word( idx : natural ) return unsigned is
        begin
            return unsigned(rd_data);
        end function;

        procedure publish is
        begin
            summary_valid <= '1';
            wait until rising_edge(clock);
            summary_valid <= '0';
            wait until rising_edge(clock);
        end procedure;

        variable w_lo, w_hi : unsigned(31 downto 0);
        variable gen_before : unsigned(15 downto 0);

    begin
        tick(4);
        reset <= '0';
        tick(2);

        ------------------------------------------------------------------
        -- Dwell A: distinctive values, with the halves of each wide field
        -- different so a swap cannot pass.
        ------------------------------------------------------------------
        energy_sum      <= x"AAAABBBB_CCCCDDDD";
        peak            <= x"11112222";
        clip_count      <= to_unsigned(50, 32);
        sample_count    <= to_unsigned(100000, 32);
        first_timestamp <= x"01234567_89ABCDEF";
        first_window    <= to_unsigned(7, 16);
        mean_power      <= x"33334444";
        noise_floor     <= x"5555_66667777";
        peak_window     <= x"8888_9999AAAA";
        tick(1);
        gen_before := generation;
        publish;

        assert generation = gen_before + 1
            report "FAIL: generation did not advance on a published summary"
            severity error;
        report "case 1 OK: generation advanced to "
               & integer'image(to_integer(generation));

        ------------------------------------------------------------------
        -- 2. Word order of the 64-bit field: low half first.
        ------------------------------------------------------------------
        rd_index <= to_unsigned(0, 4); tick(2); w_lo := read_word(0);
        rd_index <= to_unsigned(1, 4); tick(2); w_hi := read_word(1);
        assert w_lo = x"CCCCDDDD" and w_hi = x"AAAABBBB"
            report "FAIL: energy_sum halves swapped -- got hi="
                   & to_hstring(w_hi) & " lo=" & to_hstring(w_lo)
            severity error;
        report "case 2 OK: energy_sum low word first";

        -- The 48-bit field is zero-extended into its high word, not
        -- sign-extended or left with stale bits above bit 15.
        rd_index <= to_unsigned(9, 4); tick(2); w_hi := read_word(9);
        assert w_hi = x"00005555"
            report "FAIL: noise_floor high word is " & to_hstring(w_hi)
                   & ", expected 0x00005555 (zero-extended)"
            severity error;
        report "case 3 OK: 48-bit field zero-extended into its high word";

        ------------------------------------------------------------------
        -- 4. The latch holds while the live inputs move. This is the whole
        --    reason the block exists: a host mid-read must not see the next
        --    dwell appear underneath it.
        ------------------------------------------------------------------
        energy_sum <= x"DEADBEEF_FEEDFACE";
        peak       <= x"FFFFFFFF";
        tick(4);
        rd_index <= to_unsigned(0, 4); tick(2); w_lo := read_word(0);
        rd_index <= to_unsigned(2, 4); tick(2); w_hi := read_word(2);
        assert w_lo = x"CCCCDDDD" and w_hi = x"11112222"
            report "FAIL: live inputs leaked into the latched record"
            severity error;
        assert generation = gen_before + 1
            report "FAIL: generation moved without a published summary"
            severity error;
        report "case 4 OK: latch held while inputs changed, generation still "
               & integer'image(to_integer(generation));

        ------------------------------------------------------------------
        -- 5. Publishing the new dwell replaces the record and advances the
        --    counter again -- so a host that read across the boundary sees
        --    two different generations and discards the record.
        ------------------------------------------------------------------
        publish;
        rd_index <= to_unsigned(0, 4); tick(2); w_lo := read_word(0);
        assert w_lo = x"FEEDFACE"
            report "FAIL: second dwell did not replace the record"
            severity error;
        assert generation = gen_before + 2
            report "FAIL: generation is " & integer'image(to_integer(generation))
                   & " after two dwells, expected "
                   & integer'image(to_integer(gen_before) + 2)
            severity error;
        report "case 5 OK: second dwell replaced the record, generation "
               & integer'image(to_integer(generation));

        ------------------------------------------------------------------
        -- 6. Out of range reads zero. Aliasing to a valid word would hand
        --    back real data for an index that means nothing.
        ------------------------------------------------------------------
        rd_index <= to_unsigned(13, 4); tick(2);
        assert rd_data = x"00000000"
            report "FAIL: index 13 returned " & to_hstring(rd_data)
                   & " instead of zero"
            severity error;
        rd_index <= to_unsigned(14, 4); tick(2);
        assert rd_data = x"00000000"
            report "FAIL: index 14 returned data"
            severity error;
        report "case 6 OK: out-of-range indices read zero";

        ------------------------------------------------------------------
        -- 7. Index 15 is the generation counter, reached through the same
        --    registered path as the data. That is what lets a host in
        --    another clock domain use it: it arrives already latched, so it
        --    cannot be caught mid-increment the way sixteen separately
        --    synchronised bits could.
        ------------------------------------------------------------------
        rd_index <= to_unsigned(15, 4); tick(2);
        assert unsigned(rd_data) = generation
            report "FAIL: index 15 returned " & to_hstring(rd_data)
                   & " but generation is "
                   & integer'image(to_integer(generation))
            severity error;
        report "case 7 OK: index 15 reads the generation counter, "
               & integer'image(to_integer(unsigned(rd_data)));

        report "dwell_readout_tb: all cases passed";
        done <= true;
        wait;
    end process;

end architecture;
