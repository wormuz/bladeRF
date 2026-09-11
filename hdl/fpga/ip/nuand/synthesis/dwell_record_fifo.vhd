-- Completed dwell records, delivered atomically to the system domain.
--
-- The readout latch this replaces publishes fourteen words that the host
-- reads one at a time, checking a generation counter either side. That
-- works, but it makes the host responsible for noticing a torn record and
-- retrying -- and it puts four wide buses across a clock boundary, which
-- is what made the timing analyser stop converging.
--
-- A completed dwell record is semantically atomic: energy, peak, floor and
-- the verdict describe one measurement and mean nothing apart. So it is
-- delivered as one, through a FIFO whose read side is entirely in the
-- system domain. The host reads a record that exists or finds the FIFO
-- empty; there is no third state to protocol around.
--
-- What crosses the boundary is the FIFO's own pointers, which is a problem
-- with a known answer -- Gray-coded, one bit changing at a time -- rather
-- than fourteen words of payload.
--
-- Depth is four records. A dwell is 102.5 ms; the host polls far faster
-- than that, and if it stops polling entirely, dropping the oldest record
-- is the honest failure: the alternative is stalling the measurement side,
-- which would make the sweep depend on host timing.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity dwell_record_fifo is
    generic (
        -- Words per record. Thirteen data words plus the verdict; see
        -- dwell_readout for the layout, which this keeps unchanged so the
        -- host-side decoder does not have to move.
        WORDS_LOG2  : natural := 4;     -- 16 slots, 14 used
        -- Records held before the oldest is dropped.
        DEPTH_LOG2  : natural := 2
    );
    port (
        -- Write side: sample clock, where the measurement happens.
        wr_clock        : in  std_logic;
        wr_reset        : in  std_logic;
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
        triggered       : in  std_logic;
        measure_valid   : in  std_logic;
        gain_too_high   : in  std_logic;
        settle_elapsed  : in  unsigned(15 downto 0);

        -- Read side: system clock, where the host register window lives.
        rd_clock        : in  std_logic;
        rd_reset        : in  std_logic;
        rd_index        : in  unsigned(WORDS_LOG2-1 downto 0);
        rd_data         : out std_logic_vector(31 downto 0)
                                := (others => '0');
        -- A record is present. Reading any word while this is low returns
        -- the previous record, not an undefined one -- but the host has no
        -- reason to.
        rd_valid        : out std_logic := '0';
        -- One cycle: the host has finished with the oldest record and it
        -- may be retired.
        rd_ack          : in  std_logic;

        -- Records dropped because the host was not keeping up. Sticky, in
        -- the read domain: a sweep that silently lost measurements would
        -- look like a quiet band.
        dropped_count   : out unsigned(15 downto 0) := (others => '0')
    );
end entity;

architecture arch of dwell_record_fifo is

    constant WORDS  : natural := 2 ** WORDS_LOG2;
    constant DEPTH  : natural := 2 ** DEPTH_LOG2;

    type mem_t is array (0 to DEPTH * WORDS - 1) of std_logic_vector(31 downto 0);
    -- No initialiser: this infers block RAM, and asking Quartus to
    -- materialise the initial contents of a memory this size costs hours of
    -- synthesis for values that are meaningless until written.
    signal mem : mem_t;

    -- The record is written one word per cycle, not fourteen at once.
    --
    -- Block RAM has one write port. Assigning every word in a single cycle
    -- asks for fourteen, which Quartus can only satisfy in registers -- it
    -- synthesised to 2119 logic cells and zero RAM segments that way. A
    -- dwell is 102.5 ms and this takes fourteen cycles at 61.44 MHz, so
    -- the serialisation is free.
    signal wr_word   : unsigned(WORDS_LOG2-1 downto 0) := (others => '0');
    signal wr_busy   : std_logic := '0';

    -- Registered read address: block RAM cannot do an asynchronous read.
    signal rd_addr_q : unsigned(DEPTH_LOG2 + WORDS_LOG2 - 1 downto 0)
                           := (others => '0');

    -- The record is latched when the summary arrives and shifted out from
    -- here, so the live inputs may move on immediately.
    signal hold_energy : unsigned(63 downto 0) := (others => '0');
    signal hold_peak   : unsigned(31 downto 0) := (others => '0');
    signal hold_clips  : unsigned(31 downto 0) := (others => '0');
    signal hold_count  : unsigned(31 downto 0) := (others => '0');
    signal hold_ts     : unsigned(63 downto 0) := (others => '0');
    signal hold_win    : unsigned(15 downto 0) := (others => '0');
    signal hold_mean   : unsigned(31 downto 0) := (others => '0');
    signal hold_floor  : unsigned(47 downto 0) := (others => '0');
    signal hold_pkwin  : unsigned(47 downto 0) := (others => '0');
    signal hold_verdict: std_logic_vector(31 downto 0) := (others => '0');

    function record_word( i : natural;
                          energy : unsigned; pk : unsigned;
                          clips : unsigned; cnt : unsigned;
                          ts : unsigned; win : unsigned;
                          mean : unsigned; flr : unsigned;
                          pkwin : unsigned; verdict : std_logic_vector )
        return std_logic_vector is
    begin
        case i is
            when 0  => return std_logic_vector(energy(31 downto 0));
            when 1  => return std_logic_vector(energy(63 downto 32));
            when 2  => return std_logic_vector(pk);
            when 3  => return std_logic_vector(clips);
            when 4  => return std_logic_vector(cnt);
            when 5  => return std_logic_vector(ts(31 downto 0));
            when 6  => return std_logic_vector(ts(63 downto 32));
            when 7  => return std_logic_vector(mean);
            when 8  => return std_logic_vector(flr(31 downto 0));
            when 9  => return std_logic_vector(resize(flr(47 downto 32), 32));
            when 10 => return std_logic_vector(pkwin(31 downto 0));
            when 11 => return std_logic_vector(resize(pkwin(47 downto 32), 32));
            when 12 => return std_logic_vector(resize(win, 32));
            when 13 => return verdict;
            when others => return (31 downto 0 => '0');
        end case;
    end function;

    -- Gray-coded pointers, because only these cross the boundary. One bit
    -- changes per increment, so a pointer sampled mid-change reads as
    -- either the old or the new value -- never a third.
    signal wr_ptr_bin   : unsigned(DEPTH_LOG2 downto 0) := (others => '0');
    signal wr_ptr_gray  : unsigned(DEPTH_LOG2 downto 0) := (others => '0');
    signal rd_ptr_bin   : unsigned(DEPTH_LOG2 downto 0) := (others => '0');
    signal rd_ptr_gray  : unsigned(DEPTH_LOG2 downto 0) := (others => '0');

    signal wr_ptr_gray_rd : unsigned(DEPTH_LOG2 downto 0) := (others => '0');
    signal wr_ptr_gray_r1 : unsigned(DEPTH_LOG2 downto 0) := (others => '0');
    signal rd_ptr_gray_wr : unsigned(DEPTH_LOG2 downto 0) := (others => '0');
    signal rd_ptr_gray_w1 : unsigned(DEPTH_LOG2 downto 0) := (others => '0');

    function bin_to_gray( b : unsigned ) return unsigned is
    begin
        return b xor shift_right(b, 1);
    end function;

    function gray_to_bin( g : unsigned ) return unsigned is
        variable b : unsigned(g'range) := (others => '0');
    begin
        b(g'high) := g(g'high);
        for i in g'high - 1 downto 0 loop
            b(i) := b(i + 1) xor g(i);
        end loop;
        return b;
    end function;

begin

    -- Write side ----------------------------------------------------------

    write_proc : process( wr_clock )
        variable base : natural;
        variable rd_bin_in_wr : unsigned(DEPTH_LOG2 downto 0);
        variable full : boolean;
    begin
        if( rising_edge(wr_clock) ) then
            -- Read pointer into this domain, two stages.
            rd_ptr_gray_w1 <= rd_ptr_gray;
            rd_ptr_gray_wr <= rd_ptr_gray_w1;

            if( wr_reset = '1' ) then
                wr_ptr_bin  <= (others => '0');
                wr_ptr_gray <= (others => '0');
                wr_word     <= (others => '0');
                wr_busy     <= '0';

            elsif( summary_valid = '1' and wr_busy = '0' ) then
                -- Latch the record and start shifting it out. Taken here so
                -- the analyser's live outputs are free to move on.
                hold_energy <= energy_sum;
                hold_peak   <= peak;
                hold_clips  <= clip_count;
                hold_count  <= sample_count;
                hold_ts     <= first_timestamp;
                hold_win    <= first_window;
                hold_mean   <= mean_power;
                hold_floor  <= noise_floor;
                hold_pkwin  <= peak_window;
                hold_verdict <= std_logic_vector(settle_elapsed) & x"000"
                                & '0' & triggered & gain_too_high
                                & measure_valid;
                wr_word <= (others => '0');
                wr_busy <= '1';

            elsif( wr_busy = '1' ) then
                -- One word per cycle into the single write port. The slot
                -- is the high bits of the address, the word the low ones --
                -- no multiply, so this infers block RAM.
                mem(to_integer(wr_ptr_bin(DEPTH_LOG2-1 downto 0) & wr_word))
                    <= record_word(to_integer(wr_word),
                                   hold_energy, hold_peak, hold_clips,
                                   hold_count, hold_ts, hold_win,
                                   hold_mean, hold_floor, hold_pkwin,
                                   hold_verdict);

                if( wr_word = WORDS - 1 ) then
                    -- Record complete: publish it by advancing the pointer.
                    -- Until this moment the reader cannot see any of it,
                    -- which is what makes the delivery atomic.
                    wr_busy     <= '0';
                    wr_ptr_bin  <= wr_ptr_bin + 1;
                    wr_ptr_gray <= bin_to_gray(wr_ptr_bin + 1);
                else
                    wr_word <= wr_word + 1;
                end if;
            end if;
        end if;
    end process;

    -- Read side -----------------------------------------------------------

    read_proc : process( rd_clock )
        variable wr_bin_in_rd : unsigned(DEPTH_LOG2 downto 0);
        variable base : natural;
    begin
        if( rising_edge(rd_clock) ) then
            wr_ptr_gray_r1 <= wr_ptr_gray;
            wr_ptr_gray_rd <= wr_ptr_gray_r1;

            if( rd_reset = '1' ) then
                rd_ptr_bin    <= (others => '0');
                rd_ptr_gray   <= (others => '0');
                rd_valid      <= '0';
                dropped_count <= (others => '0');
                rd_data       <= (others => '0');

            else
                wr_bin_in_rd := gray_to_bin(wr_ptr_gray_rd);

                -- Empty when the pointers match exactly.
                if( wr_bin_in_rd = rd_ptr_bin ) then
                    rd_valid <= '0';
                else
                    rd_valid <= '1';
                end if;

                -- More than DEPTH records ahead means the writer has
                -- lapped us and the oldest is gone. Skip to the oldest
                -- record that still exists and count what was lost, rather
                -- than returning a record half overwritten.
                if( wr_bin_in_rd - rd_ptr_bin > DEPTH ) then
                    dropped_count <= dropped_count
                                     + resize(wr_bin_in_rd - rd_ptr_bin
                                              - DEPTH, 16);
                    rd_ptr_bin  <= wr_bin_in_rd - DEPTH;
                    rd_ptr_gray <= bin_to_gray(wr_bin_in_rd - DEPTH);

                elsif( rd_ack = '1' and wr_bin_in_rd /= rd_ptr_bin ) then
                    rd_ptr_bin  <= rd_ptr_bin + 1;
                    rd_ptr_gray <= bin_to_gray(rd_ptr_bin + 1);
                end if;

                -- Address registered first, memory read second.
                --
                -- Quartus said exactly why the first two attempts put this
                -- in logic: "RAM logic mem is uninferred due to
                -- asynchronous read logic". Forming the address and
                -- reading in the same cycle is an asynchronous read, and
                -- block RAM has none. Registering the address adds one
                -- cycle of latency to a register the host polls at USB
                -- rates.
                --
                -- The concatenation matters too: slot*WORDS + word hides
                -- the index behind a multiply-add and Quartus stops seeing
                -- a plain address. WORDS is a power of two, so the slot is
                -- the high bits and the word the low ones.
                rd_addr_q <= rd_ptr_bin(DEPTH_LOG2-1 downto 0) & rd_index;
                rd_data   <= mem(to_integer(rd_addr_q));
            end if;
        end if;
    end process;

end architecture;
