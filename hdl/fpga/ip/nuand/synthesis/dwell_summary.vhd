-- Per-dwell measurement summary.
--
-- The sweep visits 282 frequencies per cycle, 28.9 s round trip, and today
-- every dwell ships in full over USB whether or not anything was there. Most
-- of the band is quiet most of the time.
--
-- The obvious answer -- drop dwells below a threshold -- is the wrong one for
-- this instrument. A wrong threshold, a shifted noise floor, a narrowband
-- signal under wideband noise, a short burst, a gain transition: any of those
-- makes a real emitter vanish with nothing on the host to say it was ever
-- there. False negatives are invisible by construction, which is the worst
-- property a collector can have.
--
-- So this block measures and never discards. It emits a compact summary for
-- EVERY dwell, quiet ones included, and raises an advisory trigger. What to
-- do with that -- capture raw IQ, keep only the summary, audit a fraction of
-- quiet dwells anyway -- stays with the host, where the policy can be read,
-- argued with and changed without a fourteen-minute rebuild.
--
-- What the host gets per dwell:
--
--     energy_sum    integrated I^2+Q^2 over the dwell
--     peak          largest instantaneous I^2+Q^2
--     clip_count    samples at or past the ADC rail
--     sample_count  how many samples the sum covers
--     trigger       advisory: energy crossed threshold for long enough
--     first_window  window index where it first crossed
--
-- Division is deliberately not done here. The host divides; hardware that
-- rounds is hardware that has to be explained later.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity dwell_summary is
    generic (
        -- Samples per analysis window, a power of two so the trigger
        -- comparison needs no divider.
        WINDOW_LOG2     : natural := 10;
        -- Trigger persistence: windows over threshold, out of the last
        -- TRIGGER_OF, before the trigger is raised. Rejects single-window
        -- noise spikes without needing a filter.
        TRIGGER_K       : natural := 2;
        TRIGGER_OF      : natural := 3;
        -- ADC full scale is 2048 for the 12-bit AD9361 in this design; a
        -- component at or past 2044 counts as clipped. Same threshold the
        -- host uses, so the two agree on what clipping means.
        CLIP_THRESHOLD  : natural := 2044
    );
    port (
        clock           : in  std_logic;
        reset           : in  std_logic;

        -- Registered sample stream, tapped alongside the datapath rather
        -- than in it. Nothing here drives the FIFO write interface.
        sample          : in  sample_stream_t;

        -- Dwell boundary from the host's retune sequence. A pulse ends the
        -- current dwell and starts the next.
        dwell_start     : in  std_logic;

        -- Free-running RX timestamp, same clock domain, sampled at the
        -- moment the trigger first latches. Tie to zero if unused: the
        -- output then reads zero and first_window still says which window.
        timestamp       : in  unsigned(63 downto 0) := (others => '0');

        -- Threshold on the per-window energy sum, host-programmed. Zero
        -- disables the trigger without disabling measurement.
        threshold       : in  unsigned(47 downto 0);

        -- Summary of the dwell that just ended, valid for one cycle with
        -- summary_valid. Everything is registered.
        summary_valid   : out std_logic;
        energy_sum      : out unsigned(63 downto 0);
        peak            : out unsigned(31 downto 0);
        clip_count      : out unsigned(31 downto 0);
        sample_count    : out unsigned(31 downto 0);
        triggered       : out std_logic;
        first_window    : out unsigned(15 downto 0);

        -- Timestamp captured when the trigger first latched, in RX sample
        -- ticks. first_window locates the event to a window; this locates it
        -- to a sample, which is what correlating two receivers needs.
        -- Meaningless unless triggered is set -- reads zero otherwise.
        first_timestamp : out unsigned(63 downto 0);

        -- Derived on-chip, because the fabric can and the host cannot do it
        -- in time. All three come from the window accumulator, which exists
        -- anyway for the trigger.
        --
        --   mean_power    energy per sample: a shift, since the window is a
        --                 power of two. What "how strong" means without
        --                 first having to know how many samples there were.
        --   noise_floor   the quietest window in the dwell. With a signal
        --                 present for part of the dwell this is the part
        --                 without it, which is exactly the reference a
        --                 threshold should be relative to.
        --   peak_window   the loudest window. Against noise_floor it gives
        --                 the dwell's dynamic range for free.
        mean_power      : out unsigned(31 downto 0);
        noise_floor     : out unsigned(47 downto 0);
        peak_window     : out unsigned(47 downto 0)
    );
end entity;

architecture arch of dwell_summary is

    -- I^2 + Q^2 for signed(15 downto 0) inputs needs 33 bits: each square is
    -- at most 2^30, and the sum of two is at most 2^31.
    -- The dwell boundary must travel the same number of stages as the
    -- samples it delimits. Adding the square stage put inst_valid one clock
    -- further from the sample; leaving dwell_start where it was made the
    -- last sample of each dwell arrive after its own boundary, so it was
    -- counted into the next record. The equivalence bench reported exactly
    -- that: sample_count 47 vs 46, first_timestamp 44 vs 45, and a dwell
    -- the reference closed empty coming back with one sample in it.
    --
    -- PIPE_DEPTH is the number of register stages between sample.data_v and
    -- inst_valid: square_stage, then energy_stage. Change the stage count
    -- and this must change with it, which is what the bench is there to
    -- catch.
    constant PIPE_DEPTH     : natural := 2;
    signal dwell_start_pipe : std_logic_vector(PIPE_DEPTH-1 downto 0) := (others => '0');
    signal dwell_start_d    : std_logic := '0';

    -- The dwell boundary again, one cycle later still, for the record only.
    --
    -- win_extrema folds a window into win_min/win_max a cycle after the
    -- boundary that closed it, because it reads the registered
    -- window_done_total rather than the live sum. So the last window of a
    -- dwell is still in flight when dwell_start_d arrives: assembling the
    -- record then would publish extrema that do not include it, and
    -- clearing them then would throw it away.
    --
    -- Delaying the compare without delaying the publish was tried and did
    -- exactly that -- noise_floor 0 vs 80000, peak_window 0 vs 25920000 in
    -- the equivalence bench. The two have to move together.
    --
    -- Accumulation is NOT gated while this is pending. Publication reads the
    -- snapshot and touches no accumulator, so the new dwell counts from the
    -- cycle after the boundary exactly as the unpipelined version does.
    -- Gating it was tried four different ways and each swallowed the first
    -- sample of the new dwell -- sample_count 17 vs 16 on a stream running
    -- straight through a boundary.
    signal dwell_start_cmp  : std_logic := '0';
    signal dwell_start_pub  : std_logic := '0';

    -- Dwell totals as they stood at the boundary, held for the publish cycle.
    signal snap_energy      : unsigned(63 downto 0) := (others => '0');
    signal snap_peak        : unsigned(31 downto 0) := (others => '0');
    signal snap_clips       : unsigned(31 downto 0) := (others => '0');
    signal snap_samples     : unsigned(31 downto 0) := (others => '0');
    signal snap_windows     : unsigned(15 downto 0) := (others => '0');

    -- Stage 1: the two squares, registered before anything sums them.
    signal sq_i             : unsigned(31 downto 0) := (others => '0');
    signal sq_q             : unsigned(31 downto 0) := (others => '0');
    signal sq_valid         : std_logic := '0';
    signal sq_clip          : std_logic := '0';

    -- Stage 2: their sum. valid and clip ride one stage behind the sample
    -- they describe, which is what keeps a record internally consistent.
    signal inst_energy      : unsigned(31 downto 0) := (others => '0');
    signal inst_valid       : std_logic := '0';
    signal inst_clip        : std_logic := '0';

    -- Per-window accumulator. WINDOW_LOG2 samples of a 32-bit value fits in
    -- 32 + WINDOW_LOG2 bits; 48 covers any window up to 2^16 samples.
    -- Window sums are carried at the width they can actually reach, not 48.
    --
    -- Full scale is 2048, so instantaneous I^2+Q^2 <= 2^23, and a window of
    -- 2**WINDOW_LOG2 samples sums to at most 2^(23+WINDOW_LOG2): 34 bits at
    -- the default WINDOW_LOG2 = 10, 36 at 12, 38 at 14. Declaring 48 left
    -- fourteen bits that are always zero, and three 48-bit comparators
    -- dragged their carry chains across them for nothing.
    --
    -- Measured, so the change is worth a number rather than a feeling: at 48
    -- bits the worst path in this domain was window_done_total[3] into the
    -- first comparator, seven logic levels, 9.308 ns, slack -1.485 (job 59).
    constant WIN_BITS : natural := 24 + WINDOW_LOG2;

    signal window_sum       : unsigned(WIN_BITS-1 downto 0) := (others => '0');
    signal window_count     : unsigned(15 downto 0) := (others => '0');

    -- Dwell totals.
    signal dwell_energy     : unsigned(63 downto 0) := (others => '0');
    signal dwell_peak       : unsigned(31 downto 0) := (others => '0');
    signal dwell_clips      : unsigned(31 downto 0) := (others => '0');
    signal dwell_samples    : unsigned(31 downto 0) := (others => '0');
    signal dwell_windows    : unsigned(15 downto 0) := (others => '0');

    -- Trigger state: a shift register of "this window was over threshold",
    -- and the window index where the run began.
    signal over_history     : std_logic_vector(TRIGGER_OF-1 downto 0)
                                := (others => '0');
    signal trig_latched     : std_logic := '0';
    signal trig_window      : unsigned(15 downto 0) := (others => '0');
    signal trig_time        : unsigned(63 downto 0) := (others => '0');

    -- Quietest and loudest completed window of the dwell. The minimum
    -- starts at all ones so the first window always replaces it.
    signal win_min          : unsigned(WIN_BITS-1 downto 0) := (others => '1');
    signal win_max          : unsigned(WIN_BITS-1 downto 0) := (others => '0');

    -- Handoff from accumulate to trigger_stage:
    -- one-cycle-late copy of the window-boundary event and total, since
    -- window_done/win_total are process-local variables in accumulate and
    -- cannot be read by a sibling process.
    signal window_done_pulse : std_logic := '0';
    signal window_done_total : unsigned(WIN_BITS-1 downto 0) := (others => '0');

    -- Set when the host programmed a threshold no window sum can reach.
    -- Registered rather than compared inline: it changes only when the host
    -- writes, so it costs one OR-reduce off the critical path instead of a
    -- 14-bit compare inside the window-boundary cycle.
    signal threshold_high    : std_logic := '0';

    -- Registered comparator stage feeding win_extrema. The value travels with
    -- its flags so the update never has to re-derive which window it is.
    signal cmp_valid         : std_logic := '0';
    signal cmp_lt_min        : std_logic := '0';
    signal cmp_gt_max        : std_logic := '0';
    signal cmp_total         : unsigned(WIN_BITS-1 downto 0) := (others => '0');

    function ones( v : std_logic_vector ) return natural is
        variable n : natural := 0;
    begin
        for i in v'range loop
            if( v(i) = '1' ) then
                n := n + 1;
            end if;
        end loop;
        return n;
    end function;

begin

    -- Stage 1: instantaneous energy and clip detection.
    --
    -- Clipping is judged per COMPONENT, not on the magnitude. The magnitude
    -- of an unclipped sample reaches sqrt(2) times full scale, so a
    -- magnitude test would call healthy samples clipped -- measured at 1.328
    -- on this hardware.
    --
    -- The clip test widens to 17 bits, the squares do not. Those are two
    -- different needs: abs() of the most negative 16-bit value does not fit
    -- in 16 bits, so the comparison must have room; the product does not
    -- need it, because full scale here is 2048 and I^2+Q^2 reaches 2^23.
    -- Widening both -- which this did -- asks for 32 x 32 multipliers and
    -- was why quartus_map would not converge on the sweep revision.
    -- Split across two clocks, per the architect's staging: the multiply
    -- lands in a register of its own before anything adds to it.
    --
    -- Measured reason (hosted vs sweep, Slow 1100mV 85C Fmax, seed 3,
    -- builds 000038 and 000052): the LVDS pll_sclk domain that carries this
    -- block drops from 129.63 MHz in hosted to 97.57 MHz in sweep, setup
    -- slack +0.286 to -11.822. One clock used to hold two 16x16 multiplies,
    -- their 32-bit sum, and a 17-bit-widened clip comparison on both
    -- components -- and the next stage then added a 48-bit accumulate, a
    -- window-boundary compare, min/max and the K-of-M shift on the same
    -- edge.
    --
    -- Stage 1 here: squares only, registered.
    -- Stage 2 below: their sum, plus the clip decision.
    --
    -- valid and clip travel with the data, one stage per stage: the whole
    -- point of the equivalence bench is that context must not arrive a
    -- cycle apart from what it describes.
    dwell_boundary_delay : process( clock, reset )
    begin
        if( reset = '1' ) then
            dwell_start_pipe <= (others => '0');
        elsif( rising_edge(clock) ) then
            dwell_start_pipe <= dwell_start_pipe(PIPE_DEPTH-2 downto 0) & dwell_start;
        end if;
    end process;

    -- Bit PIPE_DEPTH-2, not PIPE_DEPTH-1: the shift register's own input
    -- register is already one of the stages. Tapping the top bit delays the
    -- boundary by PIPE_DEPTH+1 while the samples are delayed by PIPE_DEPTH,
    -- which puts one sample of every dwell into the wrong record --
    -- sample_count 15 vs 16 in the equivalence bench.
    dwell_start_d <= dwell_start_pipe(PIPE_DEPTH-2);

    threshold_guard : process( clock, reset )
    begin
        if( reset = '1' ) then
            threshold_high <= '0';
        elsif( rising_edge(clock) ) then
            if( threshold(47 downto WIN_BITS) /= 0 ) then
                threshold_high <= '1';
            else
                threshold_high <= '0';
            end if;
        end if;
    end process;

    publish_delay : process( clock, reset )
    begin
        if( reset = '1' ) then
            dwell_start_cmp <= '0';
            dwell_start_pub <= '0';
        elsif( rising_edge(clock) ) then
            -- Two stages, matching the extrema path: window_done_pulse ->
            -- win_compare -> win_extrema. Publishing any earlier assembles a
            -- record whose extrema are still one comparator behind, which is
            -- the failure the equivalence bench reports as noise_floor and
            -- peak_window reading zero.
            dwell_start_cmp <= dwell_start_d;
            dwell_start_pub <= dwell_start_cmp;
        end if;
    end process;

    -- Exactly the one cycle the reference also refuses a sample on: its
    -- boundary branch takes priority over accumulation, so a sample arriving
    -- with the boundary belongs to the new dwell.
    --
    -- One cycle, not two. Gating both dwell_start_d and dwell_start_pub was
    -- tried and swallowed a sample the reference counts -- the bench
    -- reported sample_count 17 vs 16 on a stream that runs straight through
    -- a boundary. Combinational, not registered: a registered gate rises a
    -- cycle late and lets a sample into totals already being published,
    -- which the same bench caught as 15 vs 16.

    -- Snapshot of the dwell totals, taken on the boundary itself.
    --
    -- Publication happens a cycle later (it has to wait for win_extrema to
    -- fold in the last window), but the accumulators are cleared on the
    -- boundary and start the next dwell immediately. Reading them at publish
    -- time therefore misses whatever the new dwell has already added -- the
    -- bench caught that as sample_count 17 vs 16 on a stream running
    -- straight through a boundary. Latching here decouples the two.
    dwell_snapshot : process( clock, reset )
    begin
        if( reset = '1' ) then
            snap_energy  <= (others => '0');
            snap_peak    <= (others => '0');
            snap_clips   <= (others => '0');
            snap_samples <= (others => '0');
            snap_windows <= (others => '0');
        elsif( rising_edge(clock) ) then
            if( dwell_start_d = '1' ) then
                snap_energy  <= dwell_energy;
                snap_peak    <= dwell_peak;
                snap_clips   <= dwell_clips;
                snap_samples <= dwell_samples;
                snap_windows <= dwell_windows;
            end if;
        end if;
    end process;

    -- Publication, one cycle after the boundary, from the snapshot.
    --
    -- Its own process on purpose. It used to be the first branch of the
    -- accumulate if/elsif chain, which meant the publish cycle also blocked
    -- accumulation -- and on a stream running through a boundary with no
    -- gap, that swallowed exactly one sample of the new dwell. The bench
    -- reported it as sample_count 17 vs 16, constant across four different
    -- attempts to fix it inside the chain. Publication does not touch the
    -- accumulators, so it does not belong in a chain that arbitrates them.
    --
    -- The extra cycle exists because win_extrema folds a window into
    -- win_min/win_max one cycle after the boundary that closed it; publishing
    -- on the boundary itself would miss the last window of the dwell.
    publish : process( clock, reset )
    begin
        if( reset = '1' ) then
            summary_valid <= '0';
            energy_sum    <= (others => '0');
            peak          <= (others => '0');
            clip_count    <= (others => '0');
            sample_count  <= (others => '0');
            mean_power    <= (others => '0');
            noise_floor   <= (others => '0');
            peak_window   <= (others => '0');
        elsif( rising_edge(clock) ) then
            summary_valid <= '0';
            if( dwell_start_pub = '1' ) then
                summary_valid <= '1';
                energy_sum    <= snap_energy;
                peak          <= snap_peak;
                clip_count    <= snap_clips;
                sample_count  <= snap_samples;

                -- Mean power per WINDOW, not per sample: dwell_samples is not
                -- a power of two, so per-sample would need a real divider,
                -- while per-window is a fixed shift. It is also directly
                -- comparable with noise_floor and peak_window, which are
                -- window sums too. The host can divide by the window size,
                -- which it knows.
                mean_power <= resize(shift_right(snap_energy, WINDOW_LOG2), 32);

                -- A dwell with no completed window has no floor to report.
                -- All ones would read as "very loud", the opposite of the
                -- truth, so send zero and let sample_count say why.
                if( snap_windows = 0 ) then
                    noise_floor <= (others => '0');
                    peak_window <= (others => '0');
                else
                    noise_floor <= resize(win_min, 48);
                    peak_window <= resize(win_max, 48);
                end if;
            end if;
        end if;
    end process;

    -- Window extrema, one cycle behind the boundary that closed the window.
    --
    -- Split out of the accumulate process because the chain there was
    -- 48-bit add -> 48-bit compare -> mux -> the same register, all on one
    -- edge: measured as the worst path in this domain once the CDC
    -- crossings were constrained (win_max[25] to itself, six logic levels,
    -- 9.091 ns, slack -1.265 on job 58). Reading the registered
    -- window_done_total instead of the live sum breaks add from compare.
    --
    -- Cleared on dwell_start_pub, not dwell_start_d: the clear has to wait
    -- for the same cycle the publish does, or the last window of the dwell
    -- is discarded before it has been folded in.
    -- Comparator stage, registered, one cycle ahead of the update below.
    --
    -- Splitting it out is what takes the wide compare off the path that ends
    -- at win_min/win_max: those registers now see a flag and a value, not a
    -- comparator output. Measured before this (job 59, Slow 1100mV 85C):
    -- window_done_total[3] into the first comparator, seven logic levels,
    -- 9.308 ns, slack -1.485 -- the whole remaining violation in this domain
    -- sat in these three compares.
    --
    -- Comparing against win_min/win_max as they stand a cycle earlier is
    -- correct because window boundaries are 2**WINDOW_LOG2 cycles apart:
    -- the update has long settled before the next window closes. It would be
    -- wrong only if two boundaries could arrive back to back.
    win_compare : process( clock, reset )
    begin
        if( reset = '1' ) then
            cmp_valid  <= '0';
            cmp_lt_min <= '0';
            cmp_gt_max <= '0';
            cmp_total  <= (others => '0');
        elsif( rising_edge(clock) ) then
            cmp_valid <= window_done_pulse;
            cmp_total <= window_done_total;
            if( window_done_total < win_min ) then
                cmp_lt_min <= '1';
            else
                cmp_lt_min <= '0';
            end if;
            if( window_done_total > win_max ) then
                cmp_gt_max <= '1';
            else
                cmp_gt_max <= '0';
            end if;
        end if;
    end process;

    win_extrema : process( clock, reset )
    begin
        if( reset = '1' ) then
            win_min <= (others => '1');
            win_max <= (others => '0');
        elsif( rising_edge(clock) ) then
            if( cmp_valid = '1' ) then
                if( cmp_lt_min = '1' ) then
                    win_min <= cmp_total;
                end if;
                if( cmp_gt_max = '1' ) then
                    win_max <= cmp_total;
                end if;
            elsif( dwell_start_pub = '1' ) then
                win_min <= (others => '1');
                win_max <= (others => '0');
            end if;
        end if;
    end process;

    square_stage : process( clock, reset )
    begin
        if( reset = '1' ) then
            sq_i     <= (others => '0');
            sq_q     <= (others => '0');
            sq_valid <= '0';
            sq_clip  <= '0';
        elsif( rising_edge(clock) ) then
            sq_valid <= sample.data_v;
            sq_i     <= unsigned(resize(sample.data_i * sample.data_i, 32));
            sq_q     <= unsigned(resize(sample.data_q * sample.data_q, 32));

            -- Clip is judged on the sample, so it is decided here and
            -- carried forward rather than recomputed a stage later from a
            -- value that no longer exists.
            if( abs(resize(sample.data_i, 17)) >= CLIP_THRESHOLD or
                abs(resize(sample.data_q, 17)) >= CLIP_THRESHOLD ) then
                sq_clip <= '1';
            else
                sq_clip <= '0';
            end if;
        end if;
    end process;

    energy_stage : process( clock, reset )
    begin
        if( reset = '1' ) then
            inst_energy <= (others => '0');
            inst_valid  <= '0';
            inst_clip   <= '0';
        elsif( rising_edge(clock) ) then
            inst_valid  <= sq_valid;
            inst_clip   <= sq_clip;
            inst_energy <= sq_i + sq_q;
        end if;
    end process;


    -- Stage 2: window accumulation and dwell totals.
    accumulate : process( clock, reset )
        variable window_done : boolean;
        -- The window total including the sample closing it, computed once.
        variable win_total   : unsigned(WIN_BITS-1 downto 0);
        -- window_sum + resize(inst_energy, 48) was written out twice in
        -- this process (the per-sample window_sum update, and win_total
        -- below): two syntactically distinct 48-bit adds over the same
        -- names, which the synthesiser has to prove equivalent before it
        -- can share one adder -- the same class of proof win_total's own
        -- comment already names, just missed for this one. Confirmed by
        -- running the b31754a1 revision standalone: it still stalled
        -- quartus_map even with win_total in place, because this second
        -- instance of the same expression was never hoisted.
        variable window_sum_next : unsigned(WIN_BITS-1 downto 0);
    begin
        if( reset = '1' ) then
            window_sum    <= (others => '0');
            window_count  <= (others => '0');
            dwell_energy  <= (others => '0');
            dwell_peak    <= (others => '0');
            dwell_clips   <= (others => '0');
            dwell_samples <= (others => '0');
            dwell_windows <= (others => '0');
            window_done_pulse <= '0';
            window_done_total <= (others => '0');
        elsif( rising_edge(clock) ) then
            -- outputs are driven by the publish process

            -- A dwell boundary clears the accumulators. It no longer
            -- publishes: dwell_snapshot latches the totals on this same edge
            -- and the publish process emits the record a cycle later.
            -- Checked first so a boundary is never lost to a sample arriving
            -- in the same cycle: the sample belongs to the new dwell.
            -- The boundary clears the accumulators here, exactly as the
            -- unpipelined version does, so a sample arriving with it belongs
            -- to the new dwell and the counts stay identical. What the
            -- boundary no longer does is publish: dwell_snapshot latches the
            -- totals on this same edge, and the record is assembled a cycle
            -- later from that latch, once win_extrema has folded in the last
            -- window. Clearing and publishing are separate events now, and
            -- keeping the clear on the original edge is what keeps
            -- sample_count identical to the reference.
            if( dwell_start_d = '1' ) then
                window_sum    <= (others => '0');
                window_count  <= (others => '0');
                dwell_energy  <= (others => '0');
                dwell_peak    <= (others => '0');
                dwell_clips   <= (others => '0');
                dwell_samples <= (others => '0');
                dwell_windows <= (others => '0');


            elsif( inst_valid = '1' ) then
                window_sum_next := window_sum + resize(inst_energy, WIN_BITS);
                window_sum    <= window_sum_next;
                window_count  <= window_count + 1;
                dwell_energy  <= dwell_energy + resize(inst_energy, 64);
                dwell_samples <= dwell_samples + 1;

                if( inst_clip = '1' ) then
                    dwell_clips <= dwell_clips + 1;
                end if;

                if( inst_energy > dwell_peak ) then
                    dwell_peak <= inst_energy;
                end if;

                -- Window boundary.
                window_done := (window_count = to_unsigned(2**WINDOW_LOG2 - 1,
                                                           window_count'length));
                if( window_done ) then
                    window_sum   <= (others => '0');
                    window_count <= (others => '0');
                    dwell_windows <= dwell_windows + 1;

                    -- Track the quietest and loudest completed window.
                    -- window_sum does not yet include this last sample, so
                    -- add it here as the trigger comparison does.
                    --
                    -- Computed once into a variable rather than repeated in
                    -- each expression. Written out five times in one clocked
                    -- process it is five syntactically distinct 48-bit adds,
                    -- and the synthesiser has to prove them equivalent before
                    -- it can share one adder. That proof is the work
                    -- quartus_map would not finish on this revision.
                    --
                    -- Reuses window_sum_next computed above, rather than
                    -- writing the same expression a second time here: that
                    -- second instance is what actually stalled quartus_map,
                    -- window_sum_next alone was not enough.
                    win_total := window_sum_next;

                    -- Deferring these two compares to a later stage was tried
                    -- and reverted: measured, not assumed. Moving them one
                    -- cycle out means the last window of a dwell is folded in
                    -- on the same edge that publishes the record, so it
                    -- misses it -- the equivalence bench reported
                    -- noise_floor 0 vs 80000 and peak_window 0 vs 25920000 on
                    -- dwells ending at a window boundary. Publication cannot
                    -- simply be held a cycle either: accumulate clears the
                    -- dwell totals on the same edge, so there would be
                    -- nothing left to publish.
                    --
                    -- Now done: win_extrema below compares against the
                    -- already-registered window_done_total, one cycle later,
                    -- and the dwell boundary is delayed to match so the last
                    -- window of a dwell still reaches its own record. The
                    -- earlier attempt moved the compare without moving the
                    -- clear, which is why it dropped that window.

                    -- Handed to trigger_stage below: window_done and win_total
                    -- are only valid the one cycle this branch runs, so latch
                    -- them for the sibling process to read on the next edge.
                    window_done_pulse <= '1';
                    window_done_total <= win_total;
                else
                    window_done_pulse <= '0';
                end if;
            else
                window_done_pulse <= '0';
            end if;
        end if;
    end process;

    -- Stage 3: K-of-M trigger persistence, its own process. Reads
    -- window_done_pulse/
    -- window_done_total (registered one cycle behind the window-boundary
    -- event in accumulate) instead of the window_done/win_total variables,
    -- which are process-local and cannot be shared across processes.
        trigger_stage : process( clock, reset )
            variable over        : std_logic;
            -- Computed once. Written out twice in one clocked process (the
            -- over_history assignment and the ones() argument) it is two
            -- syntactically distinct concatenations, and the synthesiser has
            -- to prove them equivalent before it can share the shift-register
            -- input -- the same class of proof that stalled quartus_map on
            -- window_sum + resize(inst_energy, 48) before it was hoisted into
            -- win_total (see accumulate above). Bisected and confirmed:
            -- job 44 (ones()/TRIGGER_K cut entirely) converged in 18:45;
            -- job 45 (only the ones() call cut, over_history/over kept)
            -- also converged in 17:46 -- so the offending proof is not
            -- ones() itself, it is this duplicated concatenation.
            variable next_history : std_logic_vector(TRIGGER_OF-1 downto 0);
        begin
            if( reset = '1' ) then
                over_history  <= (others => '0');
                trig_latched  <= '0';
                trig_window   <= (others => '0');
                trig_time     <= (others => '0');
                triggered       <= '0';
                first_window    <= (others => '0');
                first_timestamp <= (others => '0');
            elsif( rising_edge(clock) ) then
                if( dwell_start_d = '1' ) then
                    triggered       <= trig_latched;
                    first_window    <= trig_window;
                    first_timestamp <= trig_time;
                    over_history    <= (others => '0');
                    trig_latched    <= '0';
                    trig_window     <= (others => '0');
                    trig_time       <= (others => '0');
                elsif( window_done_pulse = '1' ) then
                    -- A zero threshold means "measure but never trigger",
                    -- which is how the host runs a survey before it knows
                    -- what a sensible threshold would be.
                    -- Compared at WIN_BITS, not 48, so the comparator does
                    -- not drag a carry chain across bits the sum can never
                    -- reach. The host's threshold is 48-bit, so a value
                    -- above the physical maximum has to mean "never
                    -- triggers" rather than silently truncating into a hit:
                    -- threshold_high catches exactly that.
                    if( threshold /= 0 and threshold_high = '0' and
                        window_done_total > threshold(WIN_BITS-1 downto 0) ) then
                        over := '1';
                    else
                        over := '0';
                    end if;

                    next_history := over_history(TRIGGER_OF-2 downto 0) & over;
                    over_history <= next_history;

                    if( trig_latched = '0' and
                        ones(next_history) >= TRIGGER_K ) then
                        trig_latched <= '1';
                        trig_window  <= dwell_windows;
                        -- Sampled here, at the crossing, not at the dwell
                        -- boundary: by then the timestamp has advanced by
                        -- the rest of the dwell and would name the wrong
                        -- instant. Latched once, since trig_latched gates
                        -- this branch.
                        --
                        -- Less PIPE_DEPTH: timestamp is free-running and
                        -- unpipelined, while the energy that caused this
                        -- crossing left the ADC PIPE_DEPTH clocks ago.
                        -- Without the correction the mark names the
                        -- instant the pipeline noticed, not the instant
                        -- the signal arrived, and that is the number two
                        -- receivers are correlated on. The equivalence
                        -- bench caught it as first_timestamp 44 vs 45.
                        --
                        -- The correction is PIPE_DEPTH-1, not PIPE_DEPTH:
                        -- this process reads window_done_total, which is
                        -- already registered one clock behind the window
                        -- boundary, so one of the two stages is spent
                        -- before the value arrives here. Subtracting the
                        -- full depth overshoots -- measured, the bench
                        -- then reported 44 vs 43.
                        trig_time    <= timestamp - (PIPE_DEPTH - 1);
                    end if;
                end if;
            end if;
        end process;


end architecture;
