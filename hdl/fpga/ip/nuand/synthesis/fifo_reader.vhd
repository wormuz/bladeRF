-- Copyright (c) 2017 Nuand LLC
--
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
--
-- The above copyright notice and this permission notice shall be included in
-- all copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
-- THE SOFTWARE.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_reader is
    generic (
        NUM_STREAMS           : natural                 := 1;
        FIFO_READ_THROTTLE    : natural range 0 to 255  := 1;
        FIFO_USEDW_WIDTH      : natural                 := 12;
        FIFO_DATA_WIDTH       : natural                 := 32;
        META_FIFO_USEDW_WIDTH : natural                 := 3;
        META_FIFO_DATA_WIDTH  : natural                 := 128
    );
    port (
        clock               :   in      std_logic;
        reset               :   in      std_logic;
        enable              :   in      std_logic;

        usb_speed           :   in      std_logic;
        meta_en             :   in      std_logic;
        packet_en           :   in      std_logic;
        eight_bit_mode_en   :   in      std_logic;
        highly_packed_mode_en : in      std_logic;
        timestamp           :   in      unsigned(63 downto 0);

        fifo_usedw          :   in      std_logic_vector(FIFO_USEDW_WIDTH-1 downto 0);
        fifo_read           :   buffer  std_logic := '0';
        fifo_empty          :   in      std_logic;
        fifo_data           :   in      std_logic_vector(FIFO_DATA_WIDTH-1 downto 0);
        fifo_holdoff        :   in      std_logic := '0';

        packet_control      :   out     packet_control_t;
        packet_empty        :   out     std_logic;
        packet_ready        :   in      std_logic;

        meta_fifo_usedw     :   in      std_logic_vector(META_FIFO_USEDW_WIDTH-1 downto 0);
        meta_fifo_read      :   buffer  std_logic := '0';
        meta_fifo_empty     :   in      std_logic;
        meta_fifo_data      :   in      std_logic_vector(META_FIFO_DATA_WIDTH-1 downto 0);

        in_sample_controls  :   in      sample_controls_t(0 to NUM_STREAMS-1) := (others => SAMPLE_CONTROL_DISABLE);
        out_samples         :   out     sample_streams_t(0 to NUM_STREAMS-1)  := (others => ZERO_SAMPLE);

        underflow_led       :   buffer  std_logic;
        underflow_count     :   buffer  unsigned(63 downto 0);
        underflow_duration  :   in      unsigned(15 downto 0);

        -- Speed Latch & Monitor / link epoch (Stage 2)
        link_start_toggle          :   in      std_logic := '0';
        usb_speed_mismatch         :   out     std_logic := '0';
        link_active                :   out     std_logic := '0';
        speed_latched              :   out     std_logic := '0';
        protocol_start_violation   :   out     std_logic := '0';
        link_epoch_counter         :   out     unsigned(7 downto 0) := (others => '0');

        -- Abort path / sticky transport-fault flags (Stage 3)
        link_stop_toggle           :   in      std_logic := '0';
        clear_fault_toggle         :   in      std_logic := '0';
        fault_sticky               :   out     std_logic_vector(4 downto 0) := (others => '0');
        abort_active                :   out     std_logic := '0';
        -- Proof that THIS direction consumed the current epoch toggle.
        -- epoch_ack mirrors the toggle it acted on, so the system domain can
        -- compare it against the toggle it issued; epoch_valid says an epoch
        -- was ever consumed at all. Both are needed: after reset the toggle
        -- and a zeroed ack would compare equal, and the host would read
        -- "epoch applied" before any epoch existed.
        --
        -- link_active cannot serve this purpose. It rises with the start but
        -- also drops on stop and abort while the toggle stands still, so it
        -- conflates "consumed this epoch" with "still running".
        epoch_ack                   :   out     std_logic := '0';
        epoch_valid                 :   out     std_logic := '0'
  );
end entity;

architecture simple of fifo_reader is

    constant DMA_BUF_SIZE_SS    : natural   := GPIF_BUF_SIZE_SS;
    constant DMA_BUF_SIZE_HS    : natural   := GPIF_BUF_SIZE_HS;
    constant MAX_TIMESTAMP      : unsigned(timestamp'high downto timestamp'low) := (others => '1');

    signal   dma_buf_size       : natural range DMA_BUF_SIZE_HS to DMA_BUF_SIZE_SS := DMA_BUF_SIZE_SS;
    signal   underflow_detected : std_logic := '0';

    -- Speed Latch & Monitor: FX3 samples USB speed once per RF-link epoch
    -- and never re-derives pcktSize/burstLen/dmaCfg.size afterward. The
    -- FPGA must mirror that behavior -- freeze usb_speed on each new link
    -- epoch (host-driven toggle, not a level) and flag (sticky) if the
    -- link speed changes underneath us within the same epoch, instead of
    -- silently re-sizing the DMA buffer mid-stream.
    --
    -- `enable` alone is NOT a reliable epoch boundary: it is a level that
    -- means "TX/RX datapath enabled" and can stay '1' across an FX3
    -- restart (USB reconnect, repeated stream-start) that changes speed
    -- without ever dropping enable. The host must signal a new epoch
    -- explicitly via link_start_toggle. Enable high with no epoch is ARMED
    -- (held, no fault) because stock FX3 raises enable in the same vendor
    -- command that resets the fabric; the violation is a START while
    -- enable is low.
    signal   latched_usb_speed  : std_logic := '0';  -- '0' == SS, matches DMA_BUF_SIZE_SS reset value below
    signal   speed_mismatch     : std_logic := '0';
    signal   link_active_i      : std_logic := '0';
    signal   speed_latched_i    : std_logic := '0';
    signal   protocol_violation : std_logic := '0';
    signal   epoch_counter      : unsigned(7 downto 0) := (others => '0');
    signal   link_toggle_prev   : std_logic := '0';

    -- Abort path / sticky transport-fault flags (Stage 3): mirrors
    -- fifo_writer's latch_usb_speed extension. See that file for the full
    -- rationale -- set-dominant sticky bits, registered abort_active_i,
    -- cleared only by reset / new epoch / explicit clear-fault pulse.
    constant FAULT_BIT_SPEED_MISMATCH     : natural := 0;
    constant FAULT_BIT_START_NO_PROGRESS  : natural := 1;  -- epoch started, nothing ever read
    constant FAULT_BIT_GPIF_TIMEOUT       : natural := 2;  -- reads were flowing, then stopped
    constant FAULT_BIT_PROTOCOL_ERROR     : natural := 3;
    constant FAULT_BIT_FIFO_ABORT         : natural := 4;

    signal fault_sticky_i      : std_logic_vector(4 downto 0) := (others => '0');

    -- Progress watchdogs, mirroring fifo_writer. Same two questions, same
    -- timeout, watching fifo_read instead of fifo_write:
    --
    --   start   the epoch began and nothing was ever read out of the FIFO.
    --           The link came up but the host is not supplying samples, or
    --           FX3 never armed the OUT endpoint.
    --   stall   samples were flowing and stopped.
    --
    -- Told apart by whether this epoch ever moved a sample. Without that,
    -- a TX link that never started and one that wedged report identically,
    -- and they need different actions.
    --
    -- ⛔ Both directions must report, not just RX. Leaving these undriven
    -- here means a wedged transmitter looks healthy while the receiver
    -- reports the same fault -- an asymmetry that reads as an RX problem.
    constant PROGRESS_TIMEOUT_LOG2 : natural := 22;
    signal progress_count      : unsigned(PROGRESS_TIMEOUT_LOG2 downto 0)
                                    := (others => '0');
    signal read_this_epoch     : std_logic := '0';
    signal abort_active_i      : std_logic := '0';
    signal epoch_ack_i         : std_logic := '0';
    signal epoch_valid_i       : std_logic := '0';
    signal stop_toggle_prev    : std_logic := '0';
    signal clear_toggle_prev   : std_logic := '0';

    type meta_state_t is (
        META_LOAD,
        META_WAIT,
        META_DOWNCOUNT,
        ABORTED
    );

    type meta_fsm_t is record
        state           : meta_state_t;
        dma_downcount   : natural range 0 to DMA_BUF_SIZE_SS;
        meta_pkt_sop    : std_logic;
        meta_pkt_eop    : std_logic;
        skip_padding    : std_logic;
        meta_read       : std_logic;
        meta_cache      : std_logic_vector(META_FIFO_DATA_WIDTH-1 downto 0);
        meta_p_time     : unsigned(63 downto 0);
        meta_p_time_r   : unsigned(63 downto 0);
        meta_time_go    : std_logic;
        meta_fifo_empty : std_logic;
        meta_fifo_data  : std_logic_vector(META_FIFO_DATA_WIDTH-1 downto 0);
        -- Halves of the 64-bit "timestamp >= meta_p_time" decision. A single
        -- 64-bit compare took six logic levels and 9.529 ns against an 8.000 ns
        -- period on the C8 part; splitting it into two 32-bit compares keeps
        -- each carry chain short. These are registered one cycle ahead of the
        -- comparison they feed, so they are computed against meta_p_time_r
        -- (the value that becomes meta_p_time on the next clock).
        due_hi_gt       : std_logic;
        due_hi_eq       : std_logic;
        due_lo_ge       : std_logic;
        due_sentinel    : std_logic;
    end record;

    constant META_FSM_RESET_VALUE : meta_fsm_t := (
        state           => META_LOAD,
        dma_downcount   => 0,
        meta_pkt_sop    => '0',
        meta_pkt_eop    => '0',
        skip_padding    => '0',
        meta_read       => '0',
        meta_cache      => (others => '0'),
        meta_p_time     => (others => '-'),
        meta_p_time_r   => (others => '-'),
        meta_time_go    => '0',
        meta_fifo_empty => '1',
        meta_fifo_data  => (others => '0'),
        due_hi_gt       => '0',
        due_hi_eq       => '0',
        due_lo_ge       => '0',
        due_sentinel    => '0'
    );

    signal meta_current : meta_fsm_t := META_FSM_RESET_VALUE;
    signal meta_future  : meta_fsm_t := META_FSM_RESET_VALUE;

    type fifo_state_t is (
        COMPUTE_ENABLED_CHANNELS,
        COMPUTE_OFFSETS,
        READ_PACKET,
        READ_SAMPLES,
        READ_THROTTLE,
        READ_HOLDOFF
    );

    type ch_offsets_t is array( natural range <> ) of natural range fifo_data'low to fifo_data'high;

    type fifo_fsm_t is record
        state               : fifo_state_t;
        downcount           : natural range 0 to FIFO_READ_THROTTLE;
        sample_controls_reg : sample_controls_t(in_sample_controls'range);
        enabled_channels    : natural range 0 to in_sample_controls'length;
        ch_shift            : natural range 0 to out_samples'high;
        ch_offsets          : ch_offsets_t(in_sample_controls'range);
        samples_left_init   : natural range 0 to in_sample_controls'length;
        samples_left        : natural range 0 to in_sample_controls'length;
        packet_control      : packet_control_t;
        packet_data_cache   : std_logic_vector(31 downto 0);
        fifo_read           : std_logic;
        out_samples         : sample_streams_t(out_samples'range);
        eight_bit_sample_sel: std_logic;

        -- Highly packed mode signals
        packed_buffer       : std_logic_vector(47 downto 0);
        packed_cycle        : integer range 0 to 4;
    end record;

    constant FIFO_FSM_RESET_VALUE : fifo_fsm_t := (
        state               => COMPUTE_ENABLED_CHANNELS,
        downcount           => FIFO_READ_THROTTLE,
        sample_controls_reg => (others => SAMPLE_CONTROL_DISABLE),
        enabled_channels    => 0,
        ch_shift            => 0,
        ch_offsets          => (others => 0),
        samples_left_init   => 0,
        samples_left        => 0,
        packet_control      => PACKET_CONTROL_DEFAULT,
        packet_data_cache   => (others => '0'),
        fifo_read           => '0',
        out_samples         => (others => ZERO_SAMPLE),
        eight_bit_sample_sel => '0',
        packed_buffer       => (others => '0'),
        packed_cycle        => 0
    );

    signal fifo_current : fifo_fsm_t := FIFO_FSM_RESET_VALUE;
    signal fifo_future  : fifo_fsm_t := FIFO_FSM_RESET_VALUE;

begin

    -- Throw compile/synthesis error if things don't make sense
    assert ( (in_sample_controls'low = out_samples'low) and
             (in_sample_controls'high = out_samples'high) )
        report "in_sample_controls must have same range as out_samples"
        severity failure;

    -- Speed Latch & Monitor: fix the USB speed used for buffer-size math
    -- on each new link epoch (toggle on link_start_toggle, not a level),
    -- hold it for the duration of the epoch, and sticky-flag any observed
    -- change so it can be reported via a status register without
    -- perturbing dma_buf_size mid-epoch. `enable` is checked separately
    -- as a sanity signal: rising without a prior epoch is a protocol
    -- violation (sticky, independent of speed_mismatch) -- it never
    -- "heals" the protocol or clears mismatch by itself.
    latch_usb_speed : process( clock, reset )
        variable start_link_pulse  : std_logic;
        variable stop_link_pulse   : std_logic;
        variable clear_fault_pulse : std_logic;
        variable abort_active_next : std_logic;
    begin
        if( reset = '1' ) then
            latched_usb_speed  <= '0';  -- matches DMA_BUF_SIZE_SS reset value
            speed_mismatch     <= '0';
            link_active_i      <= '0';
            speed_latched_i    <= '0';
            protocol_violation <= '0';
            epoch_counter      <= (others => '0');
            link_toggle_prev   <= '0';
            fault_sticky_i     <= (others => '0');
            progress_count     <= (others => '0');
            read_this_epoch    <= '0';
            abort_active_i     <= '0';
            stop_toggle_prev   <= '0';
            clear_toggle_prev  <= '0';
            -- epoch_valid_i MUST clear on reset. Left set, the system domain
            -- would compare a stale ack against a fresh toggle and could
            -- report an epoch applied that no direction ever consumed.
            epoch_ack_i        <= '0';
            epoch_valid_i      <= '0';
        elsif( rising_edge(clock) ) then

            start_link_pulse := '0';
            if( link_start_toggle /= link_toggle_prev ) then
                start_link_pulse := '1';
            end if;
            link_toggle_prev <= link_start_toggle;

            stop_link_pulse := '0';
            if( link_stop_toggle /= stop_toggle_prev ) then
                stop_link_pulse := '1';
            end if;
            stop_toggle_prev <= link_stop_toggle;

            clear_fault_pulse := '0';
            if( clear_fault_toggle /= clear_toggle_prev ) then
                clear_fault_pulse := '1';
            end if;
            clear_toggle_prev <= clear_fault_toggle;

            if( start_link_pulse = '1' ) then
                latched_usb_speed <= usb_speed;
                speed_mismatch    <= '0';
                link_active_i     <= '1';
                -- Mirror the toggle we just acted on, and record that an
                -- epoch has now been consumed at least once.
                epoch_ack_i       <= link_start_toggle;
                epoch_valid_i     <= '1';
                -- ⛔ Це ЗАФІКСОВАНА ШВИДКІСТЬ (0=SS, 1=HS), а не «епоха була»:
                -- хост перевіряє інваріант speed_latched == usb_speed_live, і
                -- прапорець «щось зафіксовано» зробив би його завжди хибним.
                -- Факт наявності епохи несе link_active.
                speed_latched_i   <= usb_speed;
                epoch_counter     <= epoch_counter + 1;
                -- A new epoch restarts both watchdogs from nothing-seen.
                progress_count    <= (others => '0');
                read_this_epoch   <= '0';
            elsif( link_active_i = '1' ) then
                if( usb_speed /= latched_usb_speed ) then
                    speed_mismatch <= '1';
                end if;

                -- Progress watchdog. A read clears the counter and marks
                -- the epoch as having moved data; otherwise it runs. Only
                -- while the link is up: a stopped link is not stalled.
                if( fifo_read = '1' ) then
                    progress_count  <= (others => '0');
                    read_this_epoch <= '1';
                elsif( progress_count(PROGRESS_TIMEOUT_LOG2) = '0' ) then
                    -- Saturates rather than wrapping: a wrap would clear
                    -- the evidence and re-arm the fault every 34 ms.
                    progress_count <= progress_count + 1;
                end if;
            end if;

            -- Protocol violation: an epoch declared for a dead datapath.
            -- Same rule as fifo_writer: stock FX3 resets the fabric and
            -- raises enable in one vendor command (RF_TX), so enable always
            -- precedes the START; enable high with no epoch is ARMED.
            if( start_link_pulse = '1' and enable = '0' ) then
                protocol_violation <= '1';
            end if;

            -- Sticky transport-fault flags: set-dominant, cleared only by
            -- reset / new epoch / explicit clear-fault pulse -- never by
            -- the fault condition healing.
            if( start_link_pulse = '1' ) then
                fault_sticky_i <= (others => '0');
            elsif( clear_fault_pulse = '1' ) then
                fault_sticky_i <= (others => '0');
            end if;

            if( usb_speed /= latched_usb_speed and link_active_i = '1' and start_link_pulse = '0' ) then
                fault_sticky_i(FAULT_BIT_SPEED_MISMATCH) <= '1';
            end if;

            -- Progress faults, same timeout told apart by whether this
            -- epoch ever moved a sample. Guarded on start_link_pulse = '0'
            -- so the clear at the top of a new epoch is not undone by a
            -- counter that has not been reset yet in the same cycle.
            if( link_active_i = '1' and start_link_pulse = '0' and
                progress_count(PROGRESS_TIMEOUT_LOG2) = '1' ) then
                if( read_this_epoch = '0' ) then
                    fault_sticky_i(FAULT_BIT_START_NO_PROGRESS) <= '1';
                else
                    fault_sticky_i(FAULT_BIT_GPIF_TIMEOUT) <= '1';
                end if;
            end if;

            if( start_link_pulse = '1' and enable = '0' ) then
                fault_sticky_i(FAULT_BIT_PROTOCOL_ERROR) <= '1';
            end if;

            -- What abort_active_i becomes this cycle absent a new epoch:
            -- computed from LEVEL conditions, independent of this same
            -- cycle's fault_sticky_i writes.
            abort_active_next := '0';
            if( stop_link_pulse = '1'
                or (usb_speed /= latched_usb_speed and link_active_i = '1' and start_link_pulse = '0')
                or abort_active_i = '1' ) then
                abort_active_next := '1';
            end if;

            -- FAULT_BIT_FIFO_ABORT: set on the RISING EDGE of abort_active
            -- only -- re-testing a held level every cycle would immediately
            -- re-set this bit right after a clear-fault pulse cleared it
            -- (self-referential loop).
            if( abort_active_next = '1' and abort_active_i = '0' ) then
                fault_sticky_i(FAULT_BIT_FIFO_ABORT) <= '1';
            end if;

            -- abort_active: set on any fault trigger or a stop pulse;
            -- cleared only by reset or a new epoch. A stop pulse drops
            -- link_active_i but leaves sticky faults and epoch_counter
            -- untouched so the host can read them after stop.
            if( start_link_pulse = '1' ) then
                abort_active_i <= '0';
            else
                abort_active_i <= abort_active_next;
            end if;

            if( stop_link_pulse = '1' ) then
                link_active_i <= '0';
            end if;

        end if;
    end process;

    usb_speed_mismatch       <= speed_mismatch;
    link_active               <= link_active_i;
    speed_latched             <= speed_latched_i;
    protocol_start_violation  <= protocol_violation;
    link_epoch_counter        <= epoch_counter;
    fault_sticky              <= fault_sticky_i;
    abort_active              <= abort_active_i;
    epoch_ack                 <= epoch_ack_i;
    epoch_valid               <= epoch_valid_i;

    -- Determine the DMA buffer size based on the latched USB speed
    calc_buf_size : process( clock, reset )
    begin
        if( reset = '1' ) then
            dma_buf_size <= DMA_BUF_SIZE_SS;
        elsif( rising_edge(clock) ) then
            if( latched_usb_speed = '0' ) then
                dma_buf_size <= DMA_BUF_SIZE_SS;
            else
                dma_buf_size <= DMA_BUF_SIZE_HS;
            end if;
        end if;
    end process;


    -- ------------------------------------------------------------------------
    -- META FIFO FSM
    -- ------------------------------------------------------------------------

    -- Meta FIFO synchronous process
    meta_fsm_sync : process( clock, reset )
    begin
        if( reset = '1' ) then
            meta_current <= META_FSM_RESET_VALUE;
        elsif( rising_edge(clock) ) then
            meta_current <= meta_future;
        end if;
    end process;

    packet_empty <= '1' when ( meta_current.meta_fifo_empty = '1' and meta_current.state /= META_WAIT ) else '0' ;

    -- Meta FIFO combinatorial process
    meta_fsm_comb : process( all )
        constant  META_NOW      : unsigned(63 downto 0) := (others => '1');
        variable  meta_time     : unsigned(63 downto 0);
        variable  packet_len    : integer;
        variable  ts_next       : unsigned(63 downto 0);
        variable  cmp_tgt       : unsigned(63 downto 0);
    begin

        meta_future <= meta_current;

        meta_future.meta_read <= '0';
        meta_future.meta_pkt_sop <= '0';
        meta_future.meta_pkt_eop <= '0';
        meta_future.meta_fifo_empty <= meta_fifo_empty;
        meta_future.meta_fifo_data  <= meta_fifo_data;
        -- Use the header the meta FIFO is presenting now, not the copy
        -- registered on the previous cycle. That copy is all zeros out of
        -- reset, and "0 - 1" wraps to all ones -- which is exactly the
        -- META_NOW sentinel. META_WAIT then sees meta_p_time =
        -- MAX_TIMESTAMP and opens the gate immediately, so a burst
        -- scheduled in the future is emitted at once and nothing is left
        -- to send when its timestamp actually arrives.
        -- ⛔ Do NOT register meta_fifo_data before this subtraction to break
        -- the "M10K output -> 64-bit borrow chain -> comparator" timing path.
        -- Tried and reverted: a registered copy is all zeros out of reset,
        -- "0 - 1" wraps to all ones, which is exactly the META_NOW sentinel,
        -- and fifo_reader_tb fails with "gate leaked: feed ran before its
        -- timestamp" (2045 reads before the target instead of 0). This is the
        -- same defect commit 441b90a9 fixed. The subtraction must consume the
        -- header the FIFO is presenting now.
        meta_time := unsigned(meta_fifo_data(95 downto 32)) - 1;
        meta_future.meta_p_time_r <= meta_time;

        -- Precompute "timestamp >= meta_p_time" one cycle early, split across
        -- the 32-bit halves so neither carry chain spans 64 bits.
        --
        -- timestamp is a free-running counter incremented by exactly 1 each
        -- clock (time_tamer.vhd), so the value it will hold when these
        -- registered results are consumed is timestamp + 1. Comparing against
        -- timestamp + 1 here therefore yields exactly the same answer the
        -- unpipelined 64-bit compare would produce on that later cycle -- the
        -- release cycle is unchanged, not delayed.
        --
        -- meta_p_time is constant for the whole of META_WAIT, and equals
        -- meta_p_time_r captured on entry, so comparing against meta_p_time_r
        -- one cycle ahead of that capture is consistent for both states.
        -- Compare against the RAW header, not against (header - 1).
        --
        -- The released condition is evaluated one cycle before it is used, so
        -- the timestamp in force when the registered result is consumed is
        -- timestamp + 1. Substituting that into the original test:
        --     (timestamp + 1) >= meta_p_time      where meta_p_time = header - 1
        --  => (timestamp + 1) >= header - 1
        --  => (timestamp + 2) >= header
        -- Hence ts_next is timestamp + 2 here, not + 1. Using + 1 releases one
        -- cycle late: the bench reported 2000 reads instead of 2001, which is
        -- the same signature as the deliberate off-by-one negative control.
        ts_next := timestamp + 2;
        -- Comparing the raw header keeps
        -- the 64-bit borrow chain of the subtraction out of this comparison's
        -- cone: on the worst seed the path was M10K memory output -> subtract
        -- -> comparator in one cycle, cell-dominated (only 32% interconnect),
        -- with 1.138 ns spent in the subtract's carry alone.
        --
        -- The subtraction itself stays where it is, feeding meta_p_time_r for
        -- the META_LOAD test and the sentinel. It must keep reading the header
        -- the FIFO presents now -- see the warning above it.
        --
        -- Sentinel: a raw header of 0 means "transmit now". Under the old form
        -- 0 - 1 wrapped to all ones and META_WAIT matched MAX_TIMESTAMP; under
        -- this form ts_next >= 0 is unconditionally true, which opens the gate
        -- on the same cycle. Both encodings are still tested below.
        cmp_tgt := unsigned(meta_fifo_data(95 downto 32));

        if( ts_next(63 downto 32) > cmp_tgt(63 downto 32) ) then
            meta_future.due_hi_gt <= '1';
        else
            meta_future.due_hi_gt <= '0';
        end if;

        if( ts_next(63 downto 32) = cmp_tgt(63 downto 32) ) then
            meta_future.due_hi_eq <= '1';
        else
            meta_future.due_hi_eq <= '0';
        end if;

        if( ts_next(31 downto 0) >= cmp_tgt(31 downto 0) ) then
            meta_future.due_lo_ge <= '1';
        else
            meta_future.due_lo_ge <= '0';
        end if;

        -- Sentinel test does not depend on the timestamp, so it stays off the
        -- critical chain. cmp_tgt is now the raw header, so "transmit now" is
        -- a header of zero here, not MAX_TIMESTAMP -- MAX_TIMESTAMP is what
        -- zero becomes after the -1 that produces meta_p_time.
        if( cmp_tgt = 0 ) then
            meta_future.due_sentinel <= '1';
        else
            meta_future.due_sentinel <= '0';
        end if;

        case meta_current.state is

            when META_LOAD =>

                meta_future.skip_padding <= '0';
                meta_future.meta_p_time <= meta_current.meta_p_time_r;
                meta_future.meta_cache  <= meta_current.meta_fifo_data;

                if( meta_current.dma_downcount = NUM_STREAMS ) then
                    meta_future.dma_downcount <= 0;
                end if;

                if( fifo_current.ch_shift = 0 ) then
                    if( meta_current.meta_fifo_empty = '0' and (packet_en = '0' or
                               (packet_en = '1' and packet_ready = '1') ) ) then
                       meta_future.meta_read <= '1';
                       meta_future.state     <= META_WAIT;
                       -- "not due yet" reuses the registered halves instead of
                       -- a second 64-bit compare. With H the raw header,
                       --   meta_p_time_r > timestamp and /= META_NOW
                       -- is H - 1 > ts and H /= 0, i.e. H >= ts + 2, which is
                       -- exactly the negation of the due_* result computed
                       -- against timestamp + 2. Checked against the old form
                       -- over H in {0,1,2,4000} and ts around the boundary.
                       --
                       -- This matters because the old form started its borrow
                       -- chain at the meta FIFO's M10K output: on seed 7 the
                       -- worst path was memory -> Add1 -> LessThan2 -> the
                       -- fifo_read / data_v enables, at -0.502 ns.
                       if( packet_en = '1' or not (meta_current.due_hi_gt = '1'
                               or (meta_current.due_hi_eq = '1' and meta_current.due_lo_ge = '1')
                               or meta_current.due_sentinel = '1') ) then
                             meta_future.meta_time_go  <= '0';
                          else
                             meta_future.meta_time_go  <= '1';
                       end if;
                    else
                       meta_future.meta_time_go  <= '0';
                    end if;
                end if;

            when META_WAIT =>

                if( packet_en = '1' ) then
                   packet_len := to_integer(unsigned(meta_current.meta_cache(15 downto 0)));
                   meta_future.dma_downcount <= packet_len - 1;
                else
                   meta_future.dma_downcount <= dma_buf_size - 4;
                end if;

                -- (timestamp >= meta_p_time) rebuilt from the registered
                -- halves: high half greater, or high half equal and low half
                -- greater-or-equal. Identical truth value to the 64-bit
                -- compare, computed a cycle earlier against timestamp + 1.
                if( ( meta_current.due_hi_gt = '1'
                      or (meta_current.due_hi_eq = '1' and meta_current.due_lo_ge = '1')
                      or meta_current.due_sentinel = '1' )
                        and ( packet_en = '0' or ( packet_en = '1' and packet_ready = '1' ) ) ) then
                    meta_future.meta_time_go <= '1';
                    meta_future.state        <= META_DOWNCOUNT;
                    meta_future.meta_pkt_sop <= '1';
                else
                    meta_future.meta_time_go <= '0';
                end if;

            when META_DOWNCOUNT =>

                meta_future.meta_time_go  <= '1';
                if( packet_en = '0' ) then
                   if( fifo_current.fifo_read = '1') then
                      meta_future.dma_downcount <= meta_current.dma_downcount - NUM_STREAMS;
                      if( meta_current.dma_downcount <= 2 ) then
                          -- Look for 2 because of the 2 cycles passing
                          -- through META_LOAD and META_WAIT after this.
                          meta_future.state <= META_LOAD;
                      end if;
                   end if;
                else
                   if( fifo_current.packet_control.data_valid = '1') then
                      meta_future.dma_downcount <= meta_current.dma_downcount - 1;
                   end if;
                   if( meta_current.dma_downcount <= 1 and packet_ready = '1' ) then
                       meta_future.state <= META_LOAD;
                       meta_future.meta_pkt_eop <= '1';
                   end if;
                end if;

                if( meta_current.meta_cache(0) = '1' ) then
                   meta_future.skip_padding <= '1';
                end if;

            when ABORTED =>

                -- Sticky halt: only a new epoch (start pulse) leaves this
                -- state. abort_active_i clears the same cycle the start
                -- pulse registers (latch_usb_speed process).
                meta_future.meta_read <= '0';
                if( abort_active_i = '0' ) then
                    meta_future.state <= META_LOAD;
                end if;

            when others =>

                meta_future.state <= META_LOAD;

        end case;

        -- Abort?
        -- abort_active_i is registered (latch_usb_speed process); reading
        -- it here only steers meta_future.state/meta_read, which reaches
        -- the meta FIFO interface through meta_current on the FOLLOWING
        -- clock -- the output assignment below stays a plain mirror of
        -- meta_current.meta_read, unchanged.
        -- One priority chain, same as fifo_writer. Two sequential overrides
        -- stack another multiplexer on the next-state logic, and the second
        -- one wins: with the disable clause separate, dropping enable reset
        -- the FSM straight out of ABORTED without a new epoch, which defeats
        -- the sticky halt. Abort wins.
        if( abort_active_i = '1' ) then
            meta_future.meta_read <= '0';
            meta_future.state     <= ABORTED;
        elsif( (enable = '0') or (meta_en = '0') or (link_active_i = '0') ) then
            -- link_active_i = '0' with enable high is ARMED: held until START.
            meta_future <= META_FSM_RESET_VALUE;
        end if;

        -- Output assignments
        meta_fifo_read <= meta_current.meta_read;

    end process;


    -- ------------------------------------------------------------------------
    -- SAMPLE FIFO FSM
    -- ------------------------------------------------------------------------

    -- Sample FIFO synchronous process
    fifo_fsm_sync : process( clock, reset )
    begin
        if( reset = '1' ) then
            fifo_current <= FIFO_FSM_RESET_VALUE;
        elsif( rising_edge(clock) ) then
            fifo_current <= fifo_future;
        end if;
    end process;

    -- Sample FIFO combinatorial process
    fifo_fsm_comb : process( all )

        -- --------------------------------------------------------------------
        -- MIMO UNPACKER: STEP 1 of 5
        -- --------------------------------------------------------------------
        -- The sample FIFO output is a wide data bus that may contain more
        -- than one sample for a given channel. This function unpacks
        -- the bus into an array of sample streams. For example, a 2x2 MIMO
        -- design with 16-bit samples will pack its data into a 64-bit wide
        -- bus in one of the following ways:
        --      | 63:48 | 47:32 | 31:16 | 15:0 | Bits
        --   1. |   Q1  |   I1  |   Q0  |  I0  | Channels 0 & 1 enabled
        --   2. |   Q0' |   I0' |   Q0  |  I0  | Channel 0 only enabled
        --   3. |   Q1' |   I1' |   Q1  |  I1  | Channel 1 only enabled
        -- This function will return an array of length 2. The 0th
        -- element containing I0/Q0, and the 1st element I1/Q1. It is up
        -- to the state machine to select between element 0 and 1 based
        -- on which stream(s) is/are enabled.
        function unpack( c : sample_controls_t;
                         d : std_logic_vector ) return sample_streams_t is
            variable rv          : sample_streams_t(c'range);
            constant OFFSET_UNIT : natural := rv(rv'low).data_i'length +
                                              rv(rv'low).data_q'length;
            -- The following 4 constants are platform-specific and perhaps
            -- should be parameters instead. This is good enough for now.
            constant I_HIGH      : natural := 11;
            constant I_LOW       : natural := 0;
            constant Q_HIGH      : natural := 27;
            constant Q_LOW       : natural := 16;
        begin
            -- Ensure the array indices are normalized
            assert (c'low = 0) and (c'high >= c'low)
                report "Invalid range for parameter 'c'"
                severity failure;

            for i in rv'range loop
                rv(i).data_i := resize(signed(shift_right(unsigned(d),i*OFFSET_UNIT)(I_HIGH downto I_LOW)),rv(i).data_i'length);
                rv(i).data_q := resize(signed(shift_right(unsigned(d),i*OFFSET_UNIT)(Q_HIGH downto Q_LOW)),rv(i).data_q'length);
                rv(i).data_v := '0';
            end loop;

            return rv;
        end function;

        -- --------------------------------------------------------------------
        -- MIMO UNPACKER: STEP 1 of 5 (8-bit/half band mode)
        -- --------------------------------------------------------------------
        -- The sample FIFO output is a wide data bus that may contain more
        -- than one sample for a given channel. This function unpacks
        -- the bus into an array of sample streams. For example, a 2x2 MIMO
        -- design with 8-bit samples will pack its data into a 64-bit wide
        -- bus in one of the following ways:
        --      |           Sample 1            |          Sample 0            |
        --      | 63:56 | 55:48 | 47:40 | 39:32 | 31:24 | 23:16 | 15:8  | 7:0  | Bits
        --   1. |   Q1  |   I1  |   Q0  |  I0   |   Q1  |   I1  |   Q0  |  I0  | Channels 0 & 1 enabled
        --   2. |   Q0' |   I0' |   Q0  |  I0   |   Q0' |   I0' |   Q0  |  I0  | Channel 0 only enabled
        --   3. |   Q1' |   I1' |   Q1  |  I1   |   Q1' |   I1' |   Q1  |  I1  | Channel 1 only enabled
        --
        -- Note: This function is meant to be ran twice to retreive both samples on the bus
        function unpack_eight_bit_mode( c : sample_controls_t;
                                        d : std_logic_vector;
                                        sample : std_logic ) return sample_streams_t is

            variable rv : sample_streams_t(c'range);
            constant OFFSET_UNIT        : natural := 16;
            constant SAMPLE_OFFSET_UNIT : natural := 2 * OFFSET_UNIT;
            -- The following 4 constants are platform-specific and perhaps
            -- should be parameters instead. This is good enough for now.
            constant I_HIGH  :  natural := 7;
            constant I_LOW   :  natural := 0;
            constant Q_HIGH  :  natural := 15;
            constant Q_LOW   :  natural := 8;

            -- 8bit mode constants
            constant SIGMA_DELTA_BITS : signed (3 downto 0) := "0000";
        begin
            -- Ensure the array indices are normalized
            assert (c'low = 0) and (c'high >= c'low)
                report "Invalid range for parameter 'c'"
                severity failure;

            if (sample = '0') then
                for i in rv'range loop
                    rv(i).data_i := shift_left(resize(signed(shift_right(unsigned(d),i*OFFSET_UNIT)(I_HIGH downto I_LOW)), rv(i).data_i'length),4);
                    rv(i).data_q := shift_left(resize(signed(shift_right(unsigned(d),i*OFFSET_UNIT)(Q_HIGH downto Q_LOW)), rv(i).data_q'length),4);
                    rv(i).data_v := '0';
                end loop;
            elsif (sample = '1') then
                for i in rv'range loop
                    rv(i).data_i := shift_left(resize(signed(shift_left(shift_right(unsigned(d),i*OFFSET_UNIT + SAMPLE_OFFSET_UNIT)(I_HIGH downto I_LOW),0)), rv(i).data_i'length),4);
                    rv(i).data_q := shift_left(resize(signed(shift_left(shift_right(unsigned(d),i*OFFSET_UNIT + SAMPLE_OFFSET_UNIT)(Q_HIGH downto Q_LOW),0)), rv(i).data_q'length),4);
                    rv(i).data_v := '0';
                end loop;
            else
                report "fifo_reader: Sample choice out of range" severity failure;
            end if;

            return rv;
        end function;

        -- --------------------------------------------------------------------
        -- MIMO UNPACKER: STEP 1 of 5 (SC12Q11 Highly Packed Mode)
        -- --------------------------------------------------------------------
        -- The sample FIFO output is a wide data bus that contains multiple
        -- 12-bit I/Q samples packed across multiple cycles. This procedure unpacks
        -- data from a 64-bit wide bus across three cycles to extract eight 12-bit I/Q pairs.
        --
        -- The packing scheme for 12-bit samples across three 64-bit cycles:
        -- Cycle 1: | 63:60 | 59:48 | 47:36 | 35:24 | 23:12 | 11:0  | Bit indices
        --          |  Q0'  |   I0  |   Q1  |   I1  |   Q0  |  I0   | Samples
        --           [-----][--------full 12-bit samples-----------]
        --            4-bit
        --           overlap
        --
        -- Cycle 2: | 63:56 | 55:44 | 43:32 | 31:20 | 19:8  |  7:0  | Bit indices
        --          |  I1'  |   Q0  |   I0  |   Q1  |  I1   |  Q0'  | Samples
        --           [-----][--------full 12-bit samples--------][---]
        --            8-bit                                       8-bit
        --           overlap                                     overlap
        --
        -- Cycle 3: | 63:52 | 51:40 | 39:28 | 27:16 | 15:4  |  3:0  | Bit indices
        --          |   Q1  |   I1  |   Q0  |   I0  |  Q1   |  I1'  | Samples
        --           [--------full 12-bit samples-----------][-----]
        --                                                    4-bit
        --                                                   overlap
        --
        -- Note: Samples with apostrophes (') indicate partial samples that span
        -- across cycle boundaries. The complete samples are reconstructed by
        -- combining these partial segments from different cycles.
        procedure unpack_sc12q11(
            signal data : in std_logic_vector(63 downto 0);
            signal previous_data : in std_logic_vector(47 downto 0);
            signal cycle : in integer;
            variable rv : out sample_streams_t
        ) is
            constant OFFSET_UNIT : natural := 24;
        begin
            for i in rv'range loop
                rv(i).data_i := resize(signed(shift_right(unsigned(data),i*OFFSET_UNIT)(11 downto 0)),rv(i).data_i'length);
                rv(i).data_q := resize(signed(shift_right(unsigned(data),i*OFFSET_UNIT)(23 downto 12)),rv(i).data_q'length);
                rv(i).data_v := '0';
            end loop;

            if (cycle = 1) then
                rv(0).data_i := resize(signed(previous_data(43 downto 32)),rv(0).data_i'length);
                rv(0).data_q := resize(signed(data(7 downto 0) & previous_data(47 downto 44)),rv(0).data_q'length);
                rv(1).data_i := resize(signed(shift_right(unsigned(data),8)(11 downto 0)),rv(1).data_i'length);
                rv(1).data_q := resize(signed(shift_right(unsigned(data),8)(23 downto 12)),rv(1).data_q'length);
            elsif (cycle = 2) then
                rv(0).data_i := resize(signed(previous_data(27 downto 16)),rv(0).data_i'length);
                rv(0).data_q := resize(signed(previous_data(39 downto 28)),rv(0).data_q'length);
                rv(1).data_i := resize(signed(fifo_data(3 downto 0) & previous_data(47 downto 40)), rv(1).data_i'length);
                rv(1).data_q := resize(signed(fifo_data(15 downto 4)), rv(1).data_q'length);
            elsif (cycle = 3) then
                rv(0).data_i := resize(signed(previous_data(11 downto 0)),rv(0).data_i'length);
                rv(0).data_q := resize(signed(previous_data(23 downto 12)),rv(0).data_q'length);
                rv(1).data_i := resize(signed(previous_data(35 downto 24)),rv(0).data_i'length);
                rv(1).data_q := resize(signed(previous_data(47 downto 36)),rv(0).data_q'length);
            end if;
        end procedure;

        -- --------------------------------------------------------------------
        -- MIMO UNPACKER: STEP 3 of 5
        -- --------------------------------------------------------------------
        -- The FIFO data has been unpacked into an array containing I and Q
        -- samples for each of the possible channels they belong to. We need to
        -- figure out which index of this unpacked array belongs to which
        -- channel so we can output it to the correct endpoint, as follows:
        --   a. All channel indices start at 0.
        --   b. For each channel, add 1 to the index for each previous
        --      channel that is enabled. For 2x2 MIMO:
        --        ch_enabled | array index of the unpacked data stream
        --        0 & 1      | ch0 = 0    ; ch1 = 0 + 1 = 1
        --        0 only     | ch0 = 0    ; ch1 = -     = 0 (ch1 is disabled, doesn't matter)
        --        1 only     | ch0 = - = 0; ch1 = 0 + 0 = 0 (ch0 is disabled, doesn't matter)
        function compute_initial_channel_offsets( x : sample_controls_t ) return ch_offsets_t is
            variable rv : ch_offsets_t(x'range) := (others => 0);
        begin
            rv := (others => 0);
            for i in x'low+1 to x'high loop
                for j in x'low to x'high-1 loop
                    if( x(j).enable = '1' ) then
                        rv(i) := rv(i) + 1;
                    end if;
                end loop;
            end loop;
            return rv;
        end function;

        variable unpacked : sample_streams_t(out_samples'range);
        variable read_req : std_logic := '0';

    begin

        fifo_future <= fifo_current;

        fifo_future.fifo_read <= '0';
        fifo_future.packet_control.pkt_sop <= meta_current.meta_pkt_sop;

        fifo_future.packet_control.data_valid <= '0';
        -- MIMO UNPACKER: STEP 1 of 5
        if (highly_packed_mode_en = '1' and NUM_STREAMS = 2) then
            unpack_sc12q11(
                data => fifo_data,
                previous_data => fifo_current.packed_buffer,
                cycle => fifo_current.packed_cycle,
                rv => unpacked
            );
        elsif (eight_bit_mode_en = '1') then
            unpacked := unpack_eight_bit_mode(fifo_current.sample_controls_reg, fifo_data, fifo_current.eight_bit_sample_sel);
        else
            unpacked := unpack(fifo_current.sample_controls_reg, fifo_data);
        end if;

        for i in fifo_future.out_samples'range loop
            if( fifo_current.sample_controls_reg(i).enable = '1' ) then
                fifo_future.out_samples(i) <= unpacked(fifo_current.ch_offsets(i) + fifo_current.ch_shift);
            end if;
        end loop;

        case fifo_current.state is

            when COMPUTE_ENABLED_CHANNELS =>

                -- MIMO UNPACKER: STEP 2 of 5
                --   Count the number of enabled channels
                fifo_future.enabled_channels <= count_enabled_channels(in_sample_controls);

                -- Register the sample control settings
                fifo_future.sample_controls_reg <= in_sample_controls;

                fifo_future.state <= COMPUTE_OFFSETS;

            when COMPUTE_OFFSETS =>

                -- MIMO UNPACKER: STEP 3 of 5
                fifo_future.ch_offsets <= compute_initial_channel_offsets(fifo_current.sample_controls_reg);

                -- MIMO UNPACKER: STEP 4 of 5
                --   Compute the number of valid samples that each channel has remaining in fifo_data that
                --   still need to be processed (not including the first). This becomes the number of clock
                --   cycles to wait before asserting the FIFO read request to get a new batch of samples.
                fifo_future.samples_left_init <= NUM_STREAMS - fifo_current.enabled_channels;

                if (fifo_current.samples_left = FIFO_FSM_RESET_VALUE.samples_left) then
                    fifo_future.samples_left <= NUM_STREAMS - fifo_current.enabled_channels;
                    fifo_future.ch_shift     <= 0;
                end if;

                if( packet_en = '1' ) then
                    fifo_future.state <= READ_PACKET;
                    fifo_future.samples_left <= NUM_STREAMS - 1;
                else
                    fifo_future.state <= READ_SAMPLES;
                end if;

            when READ_PACKET =>
                if( meta_current.meta_pkt_eop = '1' and meta_current.skip_padding = '1') then
                    fifo_future.samples_left <= NUM_STREAMS - 1;
                end if;

                if( fifo_data'high > 31) then
                    if( fifo_current.samples_left = 0) then
                        fifo_future.packet_control.data <= fifo_current.packet_data_cache;
                    elsif( fifo_current.samples_left = 1) then
                        fifo_future.packet_control.data <= fifo_data(31 downto 0);
                        fifo_future.packet_data_cache   <= fifo_data(63 downto 32);
                    end if;
                else
                    fifo_future.packet_control.data <= fifo_data(31 downto 0);
                end if;

                fifo_future.packet_control.pkt_eop <= '0';

                if( meta_current.meta_time_go = '1' and meta_current.dma_downcount > 0 and packet_ready = '1' ) then
                    if( meta_current.dma_downcount = 1 ) then
                        fifo_future.packet_control.pkt_eop <= '1';
                    end if;
                    fifo_future.packet_control.data_valid <= '1';

                    if( fifo_current.samples_left = NUM_STREAMS - 1) then
                        fifo_future.fifo_read    <= '1';
                     end if;

                    if( fifo_current.samples_left = 0 ) then
                        fifo_future.samples_left <= NUM_STREAMS - 1;
                    else
                        fifo_future.samples_left <= fifo_current.samples_left - 1;
                    end if;
                end if;

            when READ_SAMPLES =>

                fifo_future.downcount <= FIFO_FSM_RESET_VALUE.downcount;

                if( fifo_holdoff = '1' ) then
                    -- Pause for a spell
                    fifo_future.state <= READ_HOLDOFF;

                elsif( fifo_empty = '0' and
                       (meta_en = '0' or (meta_en = '1' and meta_current.meta_time_go = '1')) ) then

                    -- Check for valid data request
                    read_req := '0';
                    for i in in_sample_controls'range loop
                        read_req := read_req or (in_sample_controls(i).data_req and in_sample_controls(i).enable);
                        fifo_future.out_samples(i).data_v <= in_sample_controls(i).data_req and
                                                             in_sample_controls(i).enable;
                    end loop;

                    -- Received a valid data request
                    if( read_req = '1' ) then
                        if( fifo_current.samples_left = 0 ) then
                            fifo_future.samples_left <= fifo_current.samples_left_init;
                            fifo_future.ch_shift     <= 0;
                            fifo_future.fifo_read    <= read_req;
                            if( eight_bit_mode_en = '1' ) then
                                fifo_future.fifo_read <= read_req and fifo_current.eight_bit_sample_sel;
                                fifo_future.eight_bit_sample_sel <= not fifo_current.eight_bit_sample_sel;
                            elsif (highly_packed_mode_en = '1' and NUM_STREAMS = 2) then
                                fifo_future.packed_cycle <= (fifo_current.packed_cycle + 1);
                                fifo_future.packed_buffer <= fifo_data(fifo_data'high downto fifo_data'high-47);
                                if fifo_current.packed_cycle = 3 then
                                    fifo_future.packed_cycle <= 0;
                                    fifo_future.fifo_read <= '0';
                                end if;
                            end if;
                        else
                            -- MIMO UNPACKER: STEP 5 of 5
                            --   Add the number of enabled channels to each channel's offset index.
                            --   This points that channel to the next valid sample in the current fifo_data.
                            fifo_future.ch_shift     <= fifo_current.ch_shift + fifo_current.enabled_channels;
                            fifo_future.samples_left <= fifo_current.samples_left - 1;
                        end if;

                        if( FIFO_FSM_RESET_VALUE.downcount /= 0 ) then
                            fifo_future.state <= READ_THROTTLE;
                        end if;
                    end if;

                end if;

            when READ_THROTTLE =>

                -- If in this state, downcount is guaranteed to be >= 1
                if( fifo_current.downcount = 1 ) then
                    fifo_future.state     <= READ_SAMPLES;
                else
                    fifo_future.downcount <= fifo_current.downcount - 1;
                end if;

            when READ_HOLDOFF =>

                if( fifo_holdoff = '0' ) then
                    fifo_future.state <= READ_SAMPLES;
                end if;

            when others =>

                fifo_future.state <= FIFO_FSM_RESET_VALUE.state;

        end case;

        -- Abort?
        -- abort_active_i is registered (latch_usb_speed process); reading it
        -- here only steers fifo_future.state/fifo_read, which reaches the
        -- output on the FOLLOWING clock through fifo_current -- the output
        -- assignment below stays a plain mirror of fifo_current, unchanged.
        if( enable = '0' or abort_active_i = '1' or link_active_i = '0' ) then
            fifo_future.fifo_read <= '0';
            fifo_future.state     <= FIFO_FSM_RESET_VALUE.state;
            for i in fifo_current.out_samples'range loop
                fifo_future.out_samples(i).data_v <= '0';
            end loop;
        end if;

        if( fifo_empty = '1' and packet_en = '0' ) then
            -- Re-evaluate the MIMO settings
            fifo_future.state <= FIFO_FSM_RESET_VALUE.state;
        end if;

        -- Output assignments
        fifo_read   <= fifo_current.fifo_read;
        out_samples <= fifo_current.out_samples;

        packet_control <= fifo_current.packet_control;

    end process;


    -- ------------------------------------------------------------------------
    -- UNDERFLOW
    -- ------------------------------------------------------------------------

    -- Underflow detection
    detect_underflows : process( clock, reset )
    begin
        if( reset = '1' ) then
            underflow_detected <= '0';
        elsif( rising_edge( clock ) ) then
            underflow_detected <= '0';
            if( enable = '1' and fifo_empty = '1' and
                (meta_en = '0' or (meta_en = '1' and meta_current.meta_time_go = '1')) ) then
                underflow_detected <= '1';
            end if;
        end if;
    end process;

    -- Count the number of times we underflow, but only if they are discontinuous
    -- meaning we have an underflow condition, a non-underflow condition, then
    -- another underflow condition counts as 2 underflows, but an underflow condition
    -- followed by N underflow conditions counts as a single underflow condition.
    count_underflows : process( clock, reset )
        variable prev_underflow : std_logic := '0';
    begin
        if( reset = '1' ) then
            prev_underflow  := '0';
            underflow_count <= (others =>'0');
        elsif( rising_edge( clock ) ) then
            if( prev_underflow = '0' and underflow_detected = '1' ) then
                underflow_count <= underflow_count + 1;
            end if;
            prev_underflow := underflow_detected;
        end if;
    end process;

    -- Active high assertion for underflow_duration when the underflow
    -- condition has been detected.  The LED will stay asserted
    -- if multiple underflows have occurred
    blink_underflow_led : process( clock, reset )
        variable downcount : natural range 0 to 2**underflow_duration'length-1 := 0;
    begin
        if( reset = '1' ) then
            downcount     := 0;
            underflow_led <= '0';
        elsif( rising_edge(clock) ) then
            -- Default to not being asserted
            underflow_led <= '0';

            -- Countdown so we can see what happened
            if( downcount /= 0 ) then
                downcount     := downcount - 1;
                underflow_led <= '1';
            end if;

            -- Underflow occurred so light it up
            if( underflow_detected = '1' ) then
                downcount := to_integer(underflow_duration);
            end if;
        end if;
    end process;

end architecture;
