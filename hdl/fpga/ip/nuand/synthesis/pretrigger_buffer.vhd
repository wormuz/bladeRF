-- Pre-trigger history buffer.
--
-- The trigger fires when the energy has already been present for TRIGGER_K
-- windows -- by construction, after the interesting part started. The onset
-- is what identifies an emitter: the preamble, the ramp, the first symbol.
-- Without history the capture always begins mid-burst.
--
-- So this keeps the last DEPTH samples in a ring, continuously, and freezes
-- them when the trigger fires. Reading is not part of the sample path: the
-- host drains the frozen ring through the Nios while the next dwell is
-- already being measured.
--
-- Sizing, against 26 free M10K on the current sweep build:
--
--     4096 samples, 32 bits each = 13 M10K = half of what is free
--     4096 / 61.44 MHz           = 67 us of history
--
-- 67 us is chosen against the thing being caught, not against the memory:
-- it covers a drone control frame's preamble and the leading edge of a
-- video burst. Doubling it would take the whole free budget and leave
-- nothing for the FFT that has to fit later.
--
-- What this is NOT: a post-trigger buffer, and not unbounded history. Both
-- are on the explicitly-rejected list in the architecture note -- the raw
-- IQ path already carries everything after the trigger, and unbounded
-- history is what the host's disk is for.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity pretrigger_buffer is
    generic (
        -- Ring depth in samples. A power of two so the wrap is a truncated
        -- counter and not a comparison.
        DEPTH_LOG2  : natural := 12
    );
    port (
        clock       : in  std_logic;
        reset       : in  std_logic;

        -- Same tapped stream the analyser sees. Writing is unconditional
        -- while armed: a ring that only records when something looks
        -- interesting has no history when it turns out something was.
        sample      : in  sample_stream_t;

        -- One cycle, from the analyser. Freezes the ring.
        trigger     : in  std_logic;

        -- Dwell boundary: re-arms and discards the previous dwell's
        -- history. History from before a retune describes a different
        -- frequency and would be worse than none.
        dwell_start : in  std_logic;

        -- Frozen: the ring holds the trigger's history and has stopped
        -- recording. Stays set until the next dwell.
        frozen      : out std_logic := '0';

        -- Where the oldest sample lives once frozen. The ring wrapped, so
        -- the read order is oldest_index, +1, ... wrapping, for DEPTH
        -- entries -- and that order is only meaningful when wrapped is set.
        oldest_index : out unsigned(DEPTH_LOG2-1 downto 0)
                            := (others => '0');
        -- False if the dwell ended before the ring filled once: then the
        -- valid history is 0 .. oldest_index-1 and nothing before it.
        wrapped      : out std_logic := '0';

        -- Read port for the host drain. Independent of the write side, so
        -- reading never disturbs recording.
        rd_addr     : in  unsigned(DEPTH_LOG2-1 downto 0)
                            := (others => '0');
        rd_data     : out std_logic_vector(31 downto 0) := (others => '0')
    );
end entity;

architecture arch of pretrigger_buffer is

    constant DEPTH : natural := 2 ** DEPTH_LOG2;

    type ring_t is array (0 to DEPTH-1) of std_logic_vector(31 downto 0);
    signal ring        : ring_t := (others => (others => '0'));

    signal wr_ptr      : unsigned(DEPTH_LOG2-1 downto 0) := (others => '0');
    signal frozen_i    : std_logic := '0';
    signal wrapped_i   : std_logic := '0';

begin

    frozen       <= frozen_i;
    wrapped      <= wrapped_i;
    -- Once wrapped, the write pointer sits on the oldest entry: it is the
    -- slot about to be overwritten. Before wrapping there is no older
    -- entry than zero.
    oldest_index <= wr_ptr when wrapped_i = '1'
                    else (others => '0');

    ring_proc : process( clock )
    begin
        if( rising_edge(clock) ) then
            -- Registered read, inferring a block RAM rather than
            -- distributed logic: 4096 x 32 in ALMs would not fit, and an
            -- unregistered read of a memory this size will not make timing.
            rd_data <= ring(to_integer(rd_addr));

            if( reset = '1' ) then
                wr_ptr    <= (others => '0');
                frozen_i  <= '0';
                wrapped_i <= '0';

            else
                -- Checked before the trigger, so a dwell boundary and a
                -- trigger in the same cycle re-arm rather than freeze: the
                -- trigger belongs to the dwell that is ending, and its
                -- history is about to be discarded anyway.
                if( dwell_start = '1' ) then
                    wr_ptr    <= (others => '0');
                    frozen_i  <= '0';
                    wrapped_i <= '0';

                elsif( frozen_i = '0' ) then
                    if( trigger = '1' ) then
                        -- Freeze on the trigger cycle. The sample arriving
                        -- with it is not written: it belongs to the
                        -- post-trigger stream, which the datapath carries.
                        frozen_i <= '1';

                    elsif( sample.data_v = '1' ) then
                        ring(to_integer(wr_ptr)) <=
                            std_logic_vector(sample.data_q) &
                            std_logic_vector(sample.data_i);
                        if( wr_ptr = DEPTH-1 ) then
                            wrapped_i <= '1';
                            wr_ptr    <= (others => '0');
                        else
                            wr_ptr <= wr_ptr + 1;
                        end if;
                    end if;
                end if;
            end if;
        end if;
    end process;

end architecture;
