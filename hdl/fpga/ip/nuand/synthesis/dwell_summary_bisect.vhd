-- Structural bisect of dwell_summary, per the architect's decomposition
-- after E1/E2/E3c: E3c proved a live consumer of adc_streams(0) alone does
-- not trigger the quartus_map pathology, so the cause is in dwell_summary's
-- own arithmetic cone seeing non-constant-foldable data. This variant adds
-- back ONE stage at a time; only ever one is instantiated per build via the
-- BISECT_STAGE generic, each removing a real cone through generate, not an
-- "if false" that still leaves the RTL for the synthesiser to see.
--
--   0  register the input only, no arithmetic
--   1  + I^2/Q^2 (stage 1 energy, no clip test)
--   2  + clip test
--   3  + window accumulator (window_sum, window_count)
--   4  + dwell accumulators (energy/peak/clips/samples)
--   5  + min/max window tracking
--   6  + threshold comparator
--   7  + full K-of-M trigger (over_history, ones())
--
-- Same port list as dwell_summary so bladerf_core.vhd only needs the
-- entity name changed to swap between the two while bisecting.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity dwell_summary_bisect is
    generic (
        BISECT_STAGE    : natural := 0;
        WINDOW_LOG2     : natural := 10;
        TRIGGER_K       : natural := 2;
        TRIGGER_OF      : natural := 3;
        CLIP_THRESHOLD  : natural := 2044
    );
    port (
        clock           : in  std_logic;
        reset           : in  std_logic;
        sample          : in  sample_stream_t;
        dwell_start     : in  std_logic;
        timestamp       : in  unsigned(63 downto 0) := (others => '0');
        threshold       : in  unsigned(47 downto 0);

        summary_valid   : out std_logic;
        energy_sum      : out unsigned(63 downto 0);
        peak            : out unsigned(31 downto 0);
        clip_count      : out unsigned(31 downto 0);
        sample_count    : out unsigned(31 downto 0);
        triggered       : out std_logic;
        first_window    : out unsigned(15 downto 0);
        first_timestamp : out unsigned(63 downto 0);
        mean_power      : out unsigned(31 downto 0);
        noise_floor     : out unsigned(47 downto 0);
        peak_window     : out unsigned(47 downto 0)
    );
end entity;

architecture arch of dwell_summary_bisect is

    signal sample_q      : sample_stream_t := (data_i => (others => '0'),
                                               data_q => (others => '0'),
                                               data_v => '0');

    signal inst_energy   : unsigned(31 downto 0) := (others => '0');
    signal inst_valid    : std_logic := '0';
    signal inst_clip     : std_logic := '0';

    signal window_sum    : unsigned(47 downto 0) := (others => '0');
    signal window_count  : unsigned(15 downto 0) := (others => '0');

    signal dwell_energy  : unsigned(63 downto 0) := (others => '0');
    signal dwell_peak    : unsigned(31 downto 0) := (others => '0');
    signal dwell_clips   : unsigned(31 downto 0) := (others => '0');
    signal dwell_samples : unsigned(31 downto 0) := (others => '0');
    signal dwell_windows : unsigned(15 downto 0) := (others => '0');

    signal over_history  : std_logic_vector(TRIGGER_OF-1 downto 0) := (others => '0');
    signal trig_latched  : std_logic := '0';
    signal trig_window   : unsigned(15 downto 0) := (others => '0');
    signal trig_time     : unsigned(63 downto 0) := (others => '0');

    signal win_min       : unsigned(47 downto 0) := (others => '1');
    signal win_max       : unsigned(47 downto 0) := (others => '0');

    -- Sink so stage 0's registered-only path is not optimised away: an
    -- unused sample_q would let the synthesiser prove the whole entity
    -- constant, which defeats the point of the bisect (that is E1 again).
    signal activity_q    : std_logic := '0';

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

    -- Register the tapped sample every cycle regardless of stage, so every
    -- variant has the same live, non-constant-foldable input.
    reg_input : process( clock, reset )
    begin
        if( reset = '1' ) then
            sample_q   <= (data_i => (others => '0'),
                          data_q => (others => '0'), data_v => '0');
            activity_q <= '0';
        elsif( rising_edge(clock) ) then
            sample_q   <= sample;
            activity_q <= sample.data_v xor sample.data_i(0) xor sample.data_q(0);
        end if;
    end process;

    summary_valid   <= dwell_start;
    energy_sum      <= dwell_energy;
    peak            <= dwell_peak;
    clip_count      <= dwell_clips;
    sample_count    <= dwell_samples;
    triggered       <= trig_latched or activity_q; -- keeps activity_q live
    first_window    <= trig_window;
    first_timestamp <= trig_time;
    mean_power      <= resize(shift_right(dwell_energy, WINDOW_LOG2), 32);
    noise_floor     <= win_min;
    peak_window     <= win_max;

    gen_stage1 : if( BISECT_STAGE >= 1 ) generate
        energy_stage : process( clock, reset )
            variable i_sq  : signed(31 downto 0);
            variable q_sq  : signed(31 downto 0);
            variable mag   : unsigned(31 downto 0);
        begin
            if( reset = '1' ) then
                inst_energy <= (others => '0');
                inst_valid  <= '0';
            elsif( rising_edge(clock) ) then
                inst_valid <= sample_q.data_v;
                i_sq  := sample_q.data_i * sample_q.data_i;
                q_sq  := sample_q.data_q * sample_q.data_q;
                mag   := resize(unsigned(i_sq), 32) + resize(unsigned(q_sq), 32);
                inst_energy <= mag;
            end if;
        end process;
    end generate;

    gen_stage2 : if( BISECT_STAGE >= 2 ) generate
        clip_stage : process( clock, reset )
        begin
            if( reset = '1' ) then
                inst_clip <= '0';
            elsif( rising_edge(clock) ) then
                if( abs(resize(sample_q.data_i, 17)) >= CLIP_THRESHOLD or
                    abs(resize(sample_q.data_q, 17)) >= CLIP_THRESHOLD ) then
                    inst_clip <= '1';
                else
                    inst_clip <= '0';
                end if;
            end if;
        end process;
    end generate;

    gen_stage3 : if( BISECT_STAGE >= 3 ) generate
        window_stage : process( clock, reset )
        begin
            if( reset = '1' ) then
                window_sum   <= (others => '0');
                window_count <= (others => '0');
            elsif( rising_edge(clock) ) then
                if( dwell_start = '1' ) then
                    window_sum   <= (others => '0');
                    window_count <= (others => '0');
                elsif( inst_valid = '1' ) then
                    if( window_count = to_unsigned(2**WINDOW_LOG2 - 1, window_count'length) ) then
                        window_sum   <= (others => '0');
                        window_count <= (others => '0');
                    else
                        window_sum   <= window_sum + resize(inst_energy, 48);
                        window_count <= window_count + 1;
                    end if;
                end if;
            end if;
        end process;
    end generate;

    gen_stage4 : if( BISECT_STAGE >= 4 ) generate
        dwell_totals_stage : process( clock, reset )
        begin
            if( reset = '1' ) then
                dwell_energy  <= (others => '0');
                dwell_peak    <= (others => '0');
                dwell_clips   <= (others => '0');
                dwell_samples <= (others => '0');
            elsif( rising_edge(clock) ) then
                if( dwell_start = '1' ) then
                    dwell_energy  <= (others => '0');
                    dwell_peak    <= (others => '0');
                    dwell_clips   <= (others => '0');
                    dwell_samples <= (others => '0');
                elsif( inst_valid = '1' ) then
                    dwell_energy  <= dwell_energy + resize(inst_energy, 64);
                    dwell_samples <= dwell_samples + 1;
                    if( inst_clip = '1' ) then
                        dwell_clips <= dwell_clips + 1;
                    end if;
                    if( inst_energy > dwell_peak ) then
                        dwell_peak <= inst_energy;
                    end if;
                end if;
            end if;
        end process;
    end generate;

    gen_stage5 : if( BISECT_STAGE >= 5 ) generate
        minmax_stage : process( clock, reset )
            variable win_total : unsigned(47 downto 0);
            variable window_done : boolean;
        begin
            if( reset = '1' ) then
                win_min       <= (others => '1');
                win_max       <= (others => '0');
                dwell_windows <= (others => '0');
            elsif( rising_edge(clock) ) then
                if( dwell_start = '1' ) then
                    win_min       <= (others => '1');
                    win_max       <= (others => '0');
                    dwell_windows <= (others => '0');
                elsif( inst_valid = '1' ) then
                    window_done := (window_count = to_unsigned(2**WINDOW_LOG2 - 1, window_count'length));
                    if( window_done ) then
                        win_total := window_sum + resize(inst_energy, 48);
                        dwell_windows <= dwell_windows + 1;
                        if( win_total < win_min ) then
                            win_min <= win_total;
                        end if;
                        if( win_total > win_max ) then
                            win_max <= win_total;
                        end if;
                    end if;
                end if;
            end if;
        end process;
    end generate;

    gen_stage6_7 : if( BISECT_STAGE >= 6 ) generate
        trigger_stage : process( clock, reset )
            variable win_total : unsigned(47 downto 0);
            variable window_done : boolean;
            variable over : std_logic;
        begin
            if( reset = '1' ) then
                over_history <= (others => '0');
                trig_latched <= '0';
                trig_window  <= (others => '0');
                trig_time    <= (others => '0');
            elsif( rising_edge(clock) ) then
                if( dwell_start = '1' ) then
                    over_history <= (others => '0');
                    trig_latched <= '0';
                    trig_window  <= (others => '0');
                    trig_time    <= (others => '0');
                elsif( inst_valid = '1' ) then
                    window_done := (window_count = to_unsigned(2**WINDOW_LOG2 - 1, window_count'length));
                    if( window_done ) then
                        win_total := window_sum + resize(inst_energy, 48);
                        if( threshold /= 0 and win_total > threshold ) then
                            over := '1';
                        else
                            over := '0';
                        end if;

                        if( BISECT_STAGE >= 7 ) then
                            over_history <= over_history(TRIGGER_OF-2 downto 0) & over;
                            if( trig_latched = '0' and
                                ones(over_history(TRIGGER_OF-2 downto 0) & over) >= TRIGGER_K ) then
                                trig_latched <= '1';
                                trig_window  <= dwell_windows;
                                trig_time    <= timestamp;
                            end if;
                        else
                            -- Stage 6: comparator only, no persistence/K-of-M.
                            trig_latched <= over;
                        end if;
                    end if;
                end if;
            end if;
        end process;
    end generate;

end architecture;
