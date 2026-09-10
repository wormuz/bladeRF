-- Gain settling sequencer for the sweep.
--
-- This block does NOT control gain. The AD9361 owns that: it has manual gain
-- and AGC modes designed around its own analogue stages, and a second loop in
-- fabric fighting them would be worse than either alone. What the fabric owns
-- is TIME -- knowing when the part has settled and the samples are worth
-- measuring.
--
-- The division of labour, from the architecture note:
--
--     host        picks the policy
--     FPGA        schedules the settling interval between dwells
--     AD9361      changes gain / does its fast lock
--     FPGA        holds the documented pause
--     FPGA        freezes or records the gain
--     measurement starts
--
-- So the sequencer emits one signal that matters: measure_valid. While it is
-- low the dwell is settling and any energy figure taken from it describes the
-- transient, not the band. The analyser keeps measuring regardless -- it must
-- never discard -- but the host can tell which dwells to trust.
--
-- Why not free-running AGC: it normalises output energy, so a threshold
-- calibrated on one dwell means something different on the next, and dwells
-- stop being comparable. That is the whole point of the sweep.
--
-- Budget: 28.9 s / 282 frequencies = 102.5 ms per dwell. The settle interval
-- is a few hundred microseconds, so this costs well under one percent of the
-- dwell and buys every remaining sample a defined gain state.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity gain_sequencer is
    generic (
        -- Settling time in sample clocks, counted from the dwell boundary.
        -- 8192 is ~67 us at 122.88 MHz and ~133 us at 61.44 -- longer than
        -- the AD9361 fast-lock profile recall plus its analogue settling,
        -- and a power of two so the comparison is one bit.
        SETTLE_LOG2      : natural := 13;

        -- Clip fraction above which the dwell is called unusable, in parts
        -- per million of the samples counted. 100 ppm = 0.01%, the same
        -- threshold the host applies -- a capture past it is not measurement
        -- data, so the two must agree on the number.
        CLIP_PPM_LIMIT   : natural := 100
    );
    port (
        clock            : in  std_logic;
        reset            : in  std_logic;

        -- Dwell boundary, same pulse the analyser uses.
        dwell_start      : in  std_logic;

        -- Sample strobe: settling is counted in samples, not in idle clock
        -- edges, so the interval means the same thing at either sample rate.
        sample_valid     : in  std_logic;

        -- Previous dwell's clip count and sample count, from dwell_summary.
        -- Latched on summary_valid, so the verdict below describes the dwell
        -- that just ended.
        summary_valid    : in  std_logic;
        clip_count       : in  unsigned(31 downto 0);
        sample_count     : in  unsigned(31 downto 0);

        -- Low while the dwell is settling. Not a gate on the datapath: the
        -- analyser measures the whole dwell either way, this says which part
        -- of it describes a settled receiver.
        measure_valid    : out std_logic := '0';

        -- The previous dwell clipped past the limit, so its gain was too
        -- high for this band. Advisory: the host decides whether to step
        -- gain down and revisit, and the AD9361 makes the change.
        gain_too_high    : out std_logic := '0';

        -- Settling elapsed for the current dwell, for the host to confirm
        -- the interval it asked for is the one being applied.
        settle_elapsed   : out unsigned(SETTLE_LOG2 downto 0)
                                := (others => '0')
    );
end entity;

architecture arch of gain_sequencer is

    signal settle_count : unsigned(SETTLE_LOG2 downto 0) := (others => '0');
    signal settled      : std_logic := '0';

begin

    settle_elapsed <= settle_count;
    measure_valid  <= settled;

    seq_proc : process( clock )
        -- 32-bit clip count scaled by 1e6 needs 52 bits. Sized to fit
        -- rather than trusting the count to stay small: a stuck receiver
        -- clips every sample, which is exactly when the arithmetic must
        -- not wrap and report "fine".
        variable clip_scaled : unsigned(51 downto 0);
        variable limit_term  : unsigned(51 downto 0);
    begin
        if( rising_edge(clock) ) then
            if( reset = '1' ) then
                settle_count  <= (others => '0');
                settled       <= '0';
                gain_too_high <= '0';

            else
                -- Verdict on the dwell that just ended, before the boundary
                -- clears the counter below. Compared as a product instead of
                -- a division: clip/sample > limit/1e6 becomes
                -- clip*1e6 > sample*limit, which is two multipliers and no
                -- divider. Both sides are exact, so a dwell sitting on the
                -- threshold is decided the same way every time.
                if( summary_valid = '1' ) then
                    -- resize AFTER the multiply, not before. In VHDL the
                    -- product of an n-bit and an m-bit unsigned is n+m bits
                    -- wide, so resizing the operands to 52 first yields a
                    -- 72-bit result and assigning that to a 52-bit variable
                    -- is a bound check failure -- caught by case 4 of the
                    -- testbench, not by analysis. 52 bits is the right size
                    -- for the VALUE (32 + 20 for 1e6); the intermediate just
                    -- has to be allowed to be wider before it is trimmed.
                    clip_scaled := resize(clip_count * to_unsigned(1000000, 20), 52);
                    limit_term  := resize(
                        sample_count * to_unsigned(CLIP_PPM_LIMIT, 20), 52);
                    if( sample_count = 0 ) then
                        -- No samples means no evidence, not a clean dwell.
                        -- Holding the previous verdict would attribute the
                        -- last band's clipping to this one, so clear it.
                        gain_too_high <= '0';
                    elsif( clip_scaled > limit_term ) then
                        gain_too_high <= '1';
                    else
                        gain_too_high <= '0';
                    end if;
                end if;

                -- A new dwell restarts settling. Checked after the verdict
                -- so a boundary and a summary arriving together are ordered
                -- the way they happened: the summary belongs to the dwell
                -- that is ending.
                if( dwell_start = '1' ) then
                    settle_count <= (others => '0');
                    settled      <= '0';

                elsif( settled = '0' and sample_valid = '1' ) then
                    if( settle_count(SETTLE_LOG2) = '1' ) then
                        settled <= '1';
                    else
                        settle_count <= settle_count + 1;
                    end if;
                end if;
            end if;
        end if;
    end process;

end architecture;
