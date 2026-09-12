-- Latched read window over a dwell summary.
--
-- The summary is thirteen 32-bit words and the host reads them one at a
-- time. A dwell boundary between two of those reads would hand back a
-- record that is half one dwell and half the next -- an energy from a quiet
-- band with a peak from a loud one. Nothing about the result would look
-- wrong; it would simply describe a dwell that never happened.
--
-- So the whole summary is latched on summary_valid and the host reads the
-- latch. Measurement carries on into the live signals meanwhile; only the
-- readable copy is held.
--
-- The generation counter is what makes that safe to use. The host reads it
-- before and after a record and keeps the record only if the two agree:
-- unchanged means no boundary intervened. Without it the host cannot tell a
-- consistent record from a torn one, and a torn one is indistinguishable
-- from a real measurement.
--
-- Why not a shadow-swap or a double buffer: both cost another copy of 368
-- bits and still need a counter for the host to know which half it read.
-- The counter alone is enough because the host can simply re-read.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity dwell_readout is
    port (
        clock           : in  std_logic;
        reset           : in  std_logic;

        -- One cycle per completed dwell, from dwell_summary.
        summary_valid   : in  std_logic;

        energy_sum      : in  unsigned(63 downto 0);
        peak            : in  unsigned(31 downto 0);
        clip_count      : in  unsigned(31 downto 0);
        sample_count    : in  unsigned(31 downto 0);
        first_timestamp : in  unsigned(63 downto 0);
        first_window    : in  unsigned(15 downto 0);
        mean_power      : in  unsigned(31 downto 0);
        noise_floor     : in  unsigned(47 downto 0);
        peak_window     : in  unsigned(47 downto 0);

        -- Verdict bits for the same dwell. Latched here rather than read
        -- from a separate status register so that the flags and the numbers
        -- provably describe one measurement: a register read separately can
        -- be a dwell ahead of the record, which is how a quiet dwell ends up
        -- carrying the previous band's "triggered".
        triggered       : in  std_logic := '0';
        measure_valid   : in  std_logic := '0';
        gain_too_high   : in  std_logic := '0';
        settle_elapsed  : in  unsigned(15 downto 0) := (others => '0');

        -- Word select from the host. Out-of-range reads return zero rather
        -- than aliasing to a valid word, which would look like data.
        rd_index        : in  unsigned(3 downto 0);
        rd_data         : out std_logic_vector(31 downto 0)
                                := (others => '0');

        -- Advances once per latched summary. Read it either side of a
        -- record; equal means the record is consistent.
        generation      : out unsigned(15 downto 0) := (others => '0')
    );
end entity;

architecture arch of dwell_readout is

    constant WORDS : natural := 14;

    type words_t is array (0 to WORDS-1) of std_logic_vector(31 downto 0);
    signal latched : words_t := (others => (others => '0'));
    signal gen_i   : unsigned(15 downto 0) := (others => '0');

begin

    generation <= gen_i;

    latch_proc : process( clock )
    begin
        if( rising_edge(clock) ) then
            -- Registered read. An unregistered mux over thirteen words would
            -- be combinational depth on a path the fitter has no reason to
            -- prioritise, for a register the host polls at USB rates.
            -- Index 15 returns the generation counter through the same
            -- registered path as the data words. That is what makes the
            -- counter usable across a clock domain: it leaves this block as
            -- a value latched on the read clock, so the host cannot catch it
            -- mid-increment. Crossing a 16-bit counter as sixteen separate
            -- synchronised bits can yield a number that never existed.
            if( rd_index = 15 ) then
                rd_data <= std_logic_vector(resize(gen_i, 32));
            elsif( to_integer(rd_index) < WORDS ) then
                rd_data <= latched(to_integer(rd_index));
            else
                rd_data <= (others => '0');
            end if;

            if( reset = '1' ) then
                latched <= (others => (others => '0'));
                gen_i   <= (others => '0');

            elsif( summary_valid = '1' ) then
                -- Every word from the same cycle, so the record cannot be
                -- internally inconsistent even before the generation check.
                latched(0)  <= std_logic_vector(energy_sum(31 downto 0));
                latched(1)  <= std_logic_vector(energy_sum(63 downto 32));
                latched(2)  <= std_logic_vector(peak);
                latched(3)  <= std_logic_vector(clip_count);
                latched(4)  <= std_logic_vector(sample_count);
                latched(5)  <= std_logic_vector(first_timestamp(31 downto 0));
                latched(6)  <= std_logic_vector(first_timestamp(63 downto 32));
                latched(7)  <= std_logic_vector(mean_power);
                latched(8)  <= std_logic_vector(noise_floor(31 downto 0));
                latched(9)  <= std_logic_vector(
                                   resize(noise_floor(47 downto 32), 32));
                latched(10) <= std_logic_vector(peak_window(31 downto 0));
                latched(11) <= std_logic_vector(
                                   resize(peak_window(47 downto 32), 32));
                latched(12) <= std_logic_vector(resize(first_window, 32));

                -- Word 13: this dwell's verdict, captured with its numbers
                -- so the two cannot disagree. One assignment, not several
                -- into slices of the same signal -- in a clocked process the
                -- last one wins and the earlier bits silently vanish.
                --
                --   0      measure_valid   receiver had settled
                --   1      gain_too_high   clipped past 100 ppm
                --   2      triggered       crossed the threshold
                --   15:4   reserved
                --   31:16  settle_elapsed  samples counted before settling
                latched(13) <= std_logic_vector(settle_elapsed)
                               & x"000"
                               & '0' & triggered & gain_too_high
                               & measure_valid;

                gen_i <= gen_i + 1;
            end if;
        end if;
    end process;

end architecture;
