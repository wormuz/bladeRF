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

entity fifo_writer is
    generic (
        NUM_STREAMS           : natural := 1;
        FIFO_USEDW_WIDTH      : natural := 12;
        FIFO_DATA_WIDTH       : natural := 32;
        META_FIFO_USEDW_WIDTH : natural := 5;
        META_FIFO_DATA_WIDTH  : natural := 128
    );
    port (
        clock               :   in      std_logic;
        reset               :   in      std_logic;
        enable              :   in      std_logic;

        usb_speed           :   in      std_logic;
        meta_en             :   in      std_logic;
        packet_en           :   in      std_logic;
        eight_bit_mode_en   :   in      std_logic := '0';
        highly_packed_mode_en : in      std_logic;
        timestamp           :   in      unsigned(63 downto 0);
        mini_exp            :   in      std_logic_vector(1 downto 0);

        in_sample_controls  :   in      sample_controls_t(0 to NUM_STREAMS-1) := (others => SAMPLE_CONTROL_DISABLE);
        in_samples          :   in      sample_streams_t(0 to NUM_STREAMS-1)  := (others => ZERO_SAMPLE);

        fifo_usedw          :   in      std_logic_vector(FIFO_USEDW_WIDTH-1 downto 0);
        fifo_clear          :   buffer  std_logic;
        fifo_write          :   buffer  std_logic := '0';
        fifo_full           :   in      std_logic;
        fifo_data           :   out     std_logic_vector(FIFO_DATA_WIDTH-1 downto 0) := (others => '0');

        packet_control      :   in      packet_control_t;
        packet_ready        :   out     std_logic;

        meta_fifo_full      :   in     std_logic;
        meta_fifo_usedw     :   in     std_logic_vector(META_FIFO_USEDW_WIDTH-1 downto 0);
        meta_fifo_data      :   out    std_logic_vector(META_FIFO_DATA_WIDTH-1 downto 0) := (others => '0');
        meta_fifo_write     :   out    std_logic := '0';

        overflow_led        :   buffer  std_logic;
        overflow_count      :   buffer  unsigned(63 downto 0);
        overflow_duration   :   in      unsigned(15 downto 0);

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

architecture simple of fifo_writer is

    constant DMA_BUF_SIZE_SS   : natural   := GPIF_BUF_SIZE_SS;
    constant DMA_BUF_SIZE_HS   : natural   := GPIF_BUF_SIZE_HS;

    signal dma_buf_size        : natural range DMA_BUF_SIZE_HS to DMA_BUF_SIZE_SS := DMA_BUF_SIZE_SS;

    signal fifo_enough         : boolean   := false;
    signal overflow_detected   : std_logic := '0';

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
    -- explicitly via link_start_toggle. Enable high with no epoch is the
    -- ARMED state -- FIFO held in clear, samples discarded, no fault --
    -- because stock FX3 firmware resets the fabric and raises enable in
    -- one vendor command, so enable always precedes the START. The
    -- protocol violation is a START while enable is low.
    signal latched_usb_speed   : std_logic := '0';  -- '0' == SS, matches DMA_BUF_SIZE_SS reset value below
    signal speed_mismatch      : std_logic := '0';
    signal link_active_i       : std_logic := '0';
    signal speed_latched_i     : std_logic := '0';
    signal protocol_violation  : std_logic := '0';
    signal epoch_counter       : unsigned(7 downto 0) := (others => '0');
    signal link_toggle_prev    : std_logic := '0';

    -- Abort path / sticky transport-fault flags (Stage 3): a fault detected
    -- in this clock domain stops the transfer locally (registered FSM
    -- transition) instead of waiting for Nios to poll a status register and
    -- issue a stop. `fault_sticky` bits are set-dominant and only clear on
    -- reset, a new epoch, or an explicit clear-fault pulse -- never merely
    -- because the fault condition went away, so the host can always read
    -- what happened even after link_active has dropped.
    constant FAULT_BIT_SPEED_MISMATCH     : natural := 0;
    constant FAULT_BIT_START_NO_PROGRESS  : natural := 1;  -- epoch started, nothing ever written
    constant FAULT_BIT_GPIF_TIMEOUT       : natural := 2;  -- writes were flowing, then stopped
    constant FAULT_BIT_PROTOCOL_ERROR     : natural := 3;
    constant FAULT_BIT_FIFO_ABORT         : natural := 4;

    signal fault_sticky_i      : std_logic_vector(4 downto 0) := (others => '0');

    -- Progress watchdogs for FAULT_BIT_START_NO_PROGRESS and
    -- FAULT_BIT_GPIF_TIMEOUT. Both watch the same event -- a write into the
    -- sample FIFO -- but answer different questions:
    --
    --   start   the epoch began and nothing was ever written. The link came
    --           up but no data flows: FX3 never armed, wrong alt-setting,
    --           the endpoint is not being drained.
    --   stall   data was flowing and stopped. That is the FX3 defect we
    --           cannot fix in firmware, so the gateware has to name it.
    --
    -- Told apart by whether any write has happened this epoch. Without that
    -- distinction a silent link and a wedged one report the same fault, and
    -- they need different actions from the host.
    --
    -- The limit is in sample clocks. 2^22 is ~34 ms at 122.88 MHz and ~68 ms
    -- at 61.44 -- far longer than any legitimate gap between USB buffers,
    -- short enough that the host learns within one poll. A power of two so
    -- the comparison is one bit, not a magnitude compare.
    constant PROGRESS_TIMEOUT_LOG2 : natural := 22;
    signal progress_count      : unsigned(PROGRESS_TIMEOUT_LOG2 downto 0)
                                    := (others => '0');
    signal wrote_this_epoch    : std_logic := '0';
    signal abort_active_i      : std_logic := '0';
    signal epoch_ack_i         : std_logic := '0';
    signal epoch_valid_i       : std_logic := '0';
    signal stop_toggle_prev    : std_logic := '0';
    signal clear_toggle_prev   : std_logic := '0';

    type meta_state_t is (
        IDLE,
        META_WRITE,
        META_DOWNCOUNT,
        PACKET_WAIT_EOP,
        ABORTED
    );

    type meta_fsm_t is record
        state           : meta_state_t;
        dma_downcount   : natural range 0 to 65536;
        meta_write      : std_logic;
        meta_data       : std_logic_vector(meta_fifo_data'range);
        meta_written    : std_logic;
    end record;

    constant META_FSM_RESET_VALUE : meta_fsm_t := (
        state           => IDLE,
        dma_downcount   => 0,
        meta_write      => '0',
        meta_data       => (others => '-'),
        meta_written    => '0'
    );

    signal meta_current : meta_fsm_t := META_FSM_RESET_VALUE;
    signal meta_future  : meta_fsm_t := META_FSM_RESET_VALUE;

    type fifo_state_t is (
        CLEAR,
        WRITE_SAMPLES,
        WRITE_PACKET_PAYLOAD,
        HOLDOFF
    );

    type ch_offsets_t is array( natural range <> ) of natural range fifo_data'low to fifo_data'high;

    type fifo_fsm_t is record
        state               : fifo_state_t;
        fifo_clear          : std_logic;
        fifo_write          : std_logic;
        fifo_data           : unsigned(fifo_data'range);
        fifo_12b_buf        : unsigned(95 downto 0);
        write_cycle         : natural range 0 to 3;
        samples_left        : natural range 0 to in_sample_controls'length;
        in_sample_controls_r: sample_controls_t(0 to NUM_STREAMS-1);
        in_samples_r        : sample_streams_t(0 to NUM_STREAMS-1);
        eight_bit_delay     : std_logic;
    end record;

    constant FIFO_FSM_RESET_VALUE : fifo_fsm_t := (
        state               => CLEAR,
        fifo_clear          => '1',
        fifo_write          => '0',
        fifo_data           => (others => '-'),
        fifo_12b_buf        => (others => '-'),
        write_cycle         => 0,
        samples_left        => 0,
        in_sample_controls_r=> (others => SAMPLE_CONTROL_DISABLE),
        in_samples_r        => (others => ZERO_SAMPLE),
        eight_bit_delay     => '0'
    );

    signal fifo_current : fifo_fsm_t := FIFO_FSM_RESET_VALUE;
    signal fifo_future  : fifo_fsm_t := FIFO_FSM_RESET_VALUE;

    signal sync_mini_exp: std_logic_vector(1 downto 0);

    signal meta_fifo_used_v_r : unsigned(meta_fifo_usedw'length downto 0) := (others => '0');
    signal fifo_used_v_r      : unsigned(fifo_usedw'length downto 0) := (others => '0');

begin

    -- Throw an error if port widths don't make sense
    assert (fifo_data'length >= (NUM_STREAMS*2*in_samples(in_samples'low).data_i'length) )
        report "fifo_data port width too narrow to support " & integer'image(NUM_STREAMS) & " MIMO streams."
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
            wrote_this_epoch   <= '0';
            abort_active_i     <= '0';
            stop_toggle_prev   <= '0';
            clear_toggle_prev  <= '0';
            -- epoch_valid_i MUST clear on reset. Left set, the system domain
            -- would compare a stale ack against a fresh toggle and could
            -- report an epoch applied that no direction ever consumed.
            epoch_ack_i        <= '0';
            epoch_valid_i      <= '0';
        elsif( rising_edge(clock) ) then

            -- The START toggle is shared by both directions (one host
            -- command, one epoch -- no split-brain). A direction whose
            -- enable is low is not part of that epoch and ignores the edge
            -- entirely: no link_active, no ack, no fault. The prev register
            -- still tracks, so the next START is a fresh edge for it.
            -- (Architect decision, 2026-09-11: option 1, in place of a
            -- "START on a dead datapath" fault that fired on TX in every
            -- RX-only session by construction.)
            start_link_pulse := '0';
            if( link_start_toggle /= link_toggle_prev and enable = '1' ) then
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
                wrote_this_epoch  <= '0';
            elsif( link_active_i = '1' ) then
                if( usb_speed /= latched_usb_speed ) then
                    speed_mismatch <= '1';
                end if;

                -- Progress watchdog. A write clears the counter and marks
                -- the epoch as having moved data; otherwise the counter
                -- runs. Only while the link is up: a stopped link is not
                -- stalled, it is stopped, and flagging that would make the
                -- fault meaningless.
                if( fifo_write = '1' ) then
                    progress_count   <= (others => '0');
                    wrote_this_epoch <= '1';
                elsif( progress_count(PROGRESS_TIMEOUT_LOG2) = '0' ) then
                    -- Saturates instead of wrapping: once the top bit is
                    -- set the fault is latched, and a wrap would clear the
                    -- evidence and re-arm the same fault every 34 ms.
                    progress_count <= progress_count + 1;
                end if;
            end if;

            -- No protocol violation exists any more. "Enable rose without
            -- an epoch" is the only order stock FX3 allows (it resets the
            -- fabric and raises enable in one vendor command), so it is
            -- ARMED, not a fault; and "START while enable is low" is the
            -- normal case for the unused direction of a shared START, so it
            -- is ignored above. protocol_violation stays a port, held low,
            -- so the status word keeps its layout.
            protocol_violation <= '0';

            -- Sticky transport-fault flags: set-dominant, latched by their
            -- own event, cleared only by reset / new epoch / explicit
            -- clear-fault pulse -- never by the fault condition healing.
            -- A new epoch (start pulse) clears the whole vector first so a
            -- fresh link doesn't inherit the previous epoch's faults; the
            -- set terms below are checked in the same cycle and win, which
            -- only matters for FAULT_BIT_PROTOCOL_ERROR (fed from a signal
            -- that can theoretically coincide with a start pulse).
            if( start_link_pulse = '1' ) then
                fault_sticky_i <= (others => '0');
            elsif( clear_fault_pulse = '1' ) then
                fault_sticky_i <= (others => '0');
            end if;

            if( usb_speed /= latched_usb_speed and link_active_i = '1' and start_link_pulse = '0' ) then
                fault_sticky_i(FAULT_BIT_SPEED_MISMATCH) <= '1';
            end if;

            -- Progress faults. Same timeout, told apart by whether this
            -- epoch ever moved a sample: nothing written at all is a link
            -- that never started, writes that stopped is a link that
            -- wedged. Guarded on start_link_pulse = '0' like the term
            -- above, so the clear at the top of a new epoch is not undone
            -- by a counter that has not been reset yet in the same cycle.
            if( link_active_i = '1' and start_link_pulse = '0' and
                progress_count(PROGRESS_TIMEOUT_LOG2) = '1' ) then
                if( wrote_this_epoch = '0' ) then
                    fault_sticky_i(FAULT_BIT_START_NO_PROGRESS) <= '1';
                else
                    fault_sticky_i(FAULT_BIT_GPIF_TIMEOUT) <= '1';
                end if;
            end if;

            -- FAULT_BIT_PROTOCOL_ERROR is never set: see protocol_violation
            -- above. The bit position is kept so the vector layout the host
            -- decodes does not shift.

            -- What abort_active_i becomes this cycle absent a new epoch:
            -- computed from LEVEL conditions (stop pulse or any of the
            -- other fault triggers), independent of fault_sticky_i itself,
            -- so it does not depend on this same cycle's sticky writes.
            abort_active_next := '0';
            if( stop_link_pulse = '1'
                or (usb_speed /= latched_usb_speed and link_active_i = '1' and start_link_pulse = '0')
                or abort_active_i = '1' ) then
                abort_active_next := '1';
            end if;

            -- FAULT_BIT_FIFO_ABORT: set on the RISING EDGE of abort_active
            -- only, i.e. the one cycle an abort actually discards in-flight
            -- FSM state. Re-testing a held level every cycle would
            -- immediately re-set this bit right after a clear-fault pulse
            -- cleared it (self-referential loop) -- the edge form is clean.
            if( abort_active_next = '1' and abort_active_i = '0' ) then
                fault_sticky_i(FAULT_BIT_FIFO_ABORT) <= '1';
            end if;

            -- abort_active: set on any fault trigger or a stop pulse;
            -- cleared only by reset or a new epoch. A stop pulse drops
            -- link_active_i but deliberately leaves the sticky faults and
            -- epoch_counter untouched so the host can still read them
            -- after the datapath has stopped.
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

    -- Calculate whether there's enough room in the destination FIFO
    calc_fifo_free : process( clock, reset )
        constant FIFO_MAX    : natural := 2**fifo_usedw'length;
        constant META_MAX    : natural := 2**meta_fifo_usedw'length;
        variable fifo_used_v : unsigned(fifo_usedw'length downto 0) := (others => '0');
        variable meta_fifo_used_v : unsigned(meta_fifo_usedw'length downto 0) := (others => '0');
    begin
        if( reset = '1' ) then
            fifo_enough <= false;
            fifo_used_v := (others => '0');
            meta_fifo_used_v_r <= (others => '0');
            fifo_used_v_r <= (others => '0');
        elsif( rising_edge(clock) ) then
            -- the outputs of the dcfifo are not registered so give the long combinatorial
            -- data path, let's register the values. one extra clock cycle will not change
            -- the fidelity of the result. the meta_fifo represents at minimum 204 timestamps.
            -- there is no way for an off by 1 clock cycle timing to overflow the meta fifo
            -- when the buffer leaves 4 full meta buffers empty
            fifo_used_v := unsigned(fifo_full & fifo_usedw);
            fifo_used_v_r <= fifo_used_v;
            meta_fifo_used_v := unsigned(meta_fifo_full & meta_fifo_usedw);
            meta_fifo_used_v_r <= meta_fifo_used_v;

            if( fifo_full = '0' and ((FIFO_MAX - fifo_used_v_r) > ( dma_buf_size )) and
                ( ( meta_en = '1' and meta_fifo_full = '0' and ( META_MAX - 4 ) > meta_fifo_used_v_r )
                   or (meta_en = '0') ) ) then
                fifo_enough <= true;
            else
                fifo_enough <= false;
            end if;
        end if;
    end process;

    -- ------------------------------------------------------------------------
    -- MINI EXP PIN SYNCHRONIZER
    -- ------------------------------------------------------------------------
    generate_sync_mimo_rx_en : for i in mini_exp'range generate
        U_sync_mini_exp : entity work.synchronizer
            generic map (
                RESET_LEVEL         =>  '0'
                )
            port map (
                reset               =>  '0',
                clock               =>  clock,
                async               =>  mini_exp(i),
                sync                =>  sync_mini_exp(i)
            );
    end generate;

    -- ------------------------------------------------------------------------
    -- META FIFO WRITER
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

    packet_ready <= '1' when fifo_enough else '0';

    -- Meta FIFO combinatorial process
    meta_fsm_comb : process( all )
        variable packet_flags : std_logic_vector(7 downto 0);
    begin

        meta_future            <= meta_current;

        meta_future.meta_write <= '0';
        -- currently the GPIF modules overwrites the bottom 16 bits of the flags field
        if( packet_en = '0' ) then
           meta_future.meta_data  <= x"FFF" & "11" & sync_mini_exp & x"FFFF" & std_logic_vector(timestamp) & x"12344321";
        else
           packet_flags := packet_control.pkt_flags;
           meta_future.meta_data  <= x"FFF" & "11" & sync_mini_exp & x"FFFF" & std_logic_vector(timestamp) &
                          packet_control.pkt_core_id & packet_flags &
                          std_logic_vector(to_unsigned(integer(meta_current.dma_downcount), 16));
        end if;


        case meta_current.state is
            when IDLE =>

                meta_future.dma_downcount <= dma_buf_size - 4;

                if( fifo_enough ) then
                    if( packet_en = '1' ) then
                       -- use downcount to count number of DWORDS for packets

                       if( packet_control.pkt_sop = '1' ) then
                          meta_future.state  <= PACKET_WAIT_EOP;

                          -- meta is not written yet, but there should be space
                          meta_future.meta_written <= '1';

                          -- EOP and VALID must be asserted together, count the last VALID now
                          if( packet_control.data_valid = '1') then
                             meta_future.dma_downcount <= 2;
                          else
                             meta_future.dma_downcount <= 1;
                          end if;
                       end if;
                    else
                       meta_future.state  <= META_WRITE;
                    end if;
                else
                    meta_future.meta_written <= '0';
                end if;

            when META_WRITE =>

                for i in in_samples'range loop
                    if (meta_fifo_full = '0' and in_samples(i).data_v = '1') then
                        meta_future.meta_write <= '1';
                        meta_future.meta_written <= '1';
                        meta_future.state <= META_DOWNCOUNT;
                    end if;
                end loop;

            when META_DOWNCOUNT =>

                if( fifo_current.fifo_write = '1' and meta_current.meta_write = '0' ) then
                    meta_future.dma_downcount <= meta_current.dma_downcount - NUM_STREAMS;
                end if;

                if( meta_current.dma_downcount <= 2 ) then
                    -- Look for 2 because of the 2 cycles passing
                    -- through IDLE and META_WRITE after this.
                    -- 8bit format requires an additional 2 cycle delay.
                    if( eight_bit_mode_en = '1' ) then
                        if( fifo_future.eight_bit_delay = '1' and fifo_future.samples_left = 0) then
                            meta_future.state <= IDLE;
                        end if;
                    else
                        meta_future.state <= IDLE;
                    end if;
                end if;

                -- Patches the late meta write for MIMO mode
                --
                -- Reads the registered copy, not the port. The port arrives
                -- combinationally from adc_enable in the AD9361 control
                -- register bundle, through the or/and in adc_assignment_proc
                -- at the top level, and was the design-wide worst setup path
                -- (-0.636 ns into state.PACKET_WAIT_EOP / state.META_WRITE).
                -- The registered copy is written unconditionally every clock
                -- (see in_sample_controls_r below), so this lags by exactly one
                -- cycle. That is harmless here: the condition tests whether the
                -- stream is in MIMO mode, which software sets long before
                -- samples flow, and it is further gated on dma_downcount being
                -- within NUM_STREAMS + 2 of the end -- a multi-cycle window,
                -- not an exact coincidence.
                if( in_sample_controls'length = 2 and
                    fifo_current.in_sample_controls_r(0).enable = '1' and
                    fifo_current.in_sample_controls_r(1).enable = '1' and
                    eight_bit_mode_en = '0' and
                    meta_current.dma_downcount <= NUM_STREAMS + 2 )
                then
                    meta_future.state <= IDLE;
                end if;

            when PACKET_WAIT_EOP =>

                if( packet_control.data_valid = '1' ) then
                   meta_future.dma_downcount <= meta_current.dma_downcount + 1;

                   if( packet_control.pkt_eop = '1' ) then
                       meta_future.meta_write  <= '1';
                       meta_future.state       <= IDLE;
                   end if;
                end if;

            when ABORTED =>

                -- Sticky halt: only a new epoch (start pulse) leaves this
                -- state. abort_active_i clears the same cycle the start
                -- pulse registers (latch_usb_speed process), so testing it
                -- here is reading the same registered signal the "Abort?"
                -- clause below reads to enter this state -- symmetric.
                meta_future.meta_write   <= '0';
                meta_future.meta_written <= '0';
                if( abort_active_i = '0' ) then
                    meta_future.state <= IDLE;
                end if;

            when others =>

                meta_future.state <= IDLE;

        end case;

        -- Abort?
        -- abort_active_i is itself a registered signal (see latch_usb_speed
        -- process): reading it here to steer the next FSM state is the same
        -- class of dependency as reading `enable` below, and only reaches
        -- the FIFO interface through meta_current.state on the FOLLOWING
        -- clock -- meta_fifo_write/meta_fifo_data stay driven solely by
        -- meta_current.meta_write / meta_current.meta_data, unchanged.
        -- One priority chain, not two sequential overrides. Written as two
        -- separate if statements this cost -0.164 ns on the setup path
        -- meta_current.meta_written -> meta_current.state.IDLE (measured,
        -- hostedxA4-2026-09-10_04.09.16): the next-state logic ran through
        -- two more multiplexers stacked on top of the whole case statement.
        --
        -- The order matters as much as the depth. With the disable clause
        -- second it overrode the abort, so dropping enable walked the FSM
        -- out of ABORTED into IDLE without a new epoch -- exactly the sticky
        -- halt the abort exists to provide. Abort wins now.
        if( abort_active_i = '1' ) then
            meta_future.meta_write    <= '0';
            meta_future.meta_written  <= '0';
            meta_future.state         <= ABORTED;
        elsif( (enable = '0') or (meta_en = '0') or (link_active_i = '0') ) then
            -- link_active_i = '0' with enable high is ARMED: held here until
            -- the epoch arrives, same as not enabled.
            meta_future.meta_write    <= '0';
            meta_future.meta_written  <= '0';
            meta_future.state         <= IDLE;
        end if;

        -- Output assignments
        meta_fifo_write <= meta_current.meta_write;
        meta_fifo_data  <= meta_current.meta_data;

    end process;


    -- ------------------------------------------------------------------------
    -- SAMPLE FIFO WRITER
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
        -- MIMO PACKER: STEP 1 of 3
        -- --------------------------------------------------------------------
        -- This block receives samples as an array of sample streams, one
        -- element per channel. These streams need to be packed into a single,
        -- wide bus that is written to a FIFO and eventually delivered to the
        -- host. When all channels are enabled, this wide bus will contain one
        -- I/Q sample pair for each channel. When channels are disabled, the
        -- wide bus may contain multiple samples from the remaining enabled
        -- channels in order to more efficiently use the available USB bandwidth.
        -- For example, a 2x2 MIMO design with 16-bit samples will pack its data
        -- into a 64-bit wide bus in one of the following ways:
        --      | 63:48 | 47:32 | 31:16 | 15:0 | Bit indices
        --   1. |   Q1  |   I1  |   Q0  |  I0  | Channels 0 & 1 enabled
        --   2. |   Q0' |   I0' |   Q0  |  I0  | Channel 0 only enabled
        --   3. |   Q1' |   I1' |   Q1  |  I1  | Channel 1 only enabled
        function pack( sc : sample_controls_t;
                       ss : sample_streams_t;
                       d  : unsigned ) return unsigned is
            constant LEN  : natural           := ss(ss'low).data_i'length + ss(ss'low).data_q'length;
            variable rv   : unsigned(d'range) := (others => '0');
        begin
            rv := d;
            for i in sc'range loop
                if( (sc(i).enable = '1') and (ss(i).data_v = '1') ) then
                    rv := unsigned(ss(i).data_q) & unsigned(ss(i).data_i) &
                          rv(rv'high downto rv'low+LEN);
                end if;
            end loop;
            return rv;
        end function;

        -- --------------------------------------------------------------------
        -- MIMO PACKER: STEP 1 of 3 (8bit mode)
        -- --------------------------------------------------------------------
        -- This block receives samples as an array of sample streams, one
        -- element per channel. These streams need to be packed into a single,
        -- wide bus that is written to a FIFO and eventually delivered to the
        -- host. When all channels are enabled, this wide bus will contain one
        -- I/Q sample pair for each channel. When channels are disabled, the
        -- wide bus may contain multiple samples from the remaining enabled
        -- channels in order to more efficiently use the available USB bandwidth.
        -- For example, a 2x2 MIMO design with 16-bit samples will pack its data
        -- into a 64-bit wide bus in one of the following ways:
        --      |          Sample Set 1         |          Sample Set 0        |
        --      | 63:56 | 55:48 | 47:40 | 39:32 | 31:24 | 23:16 |  15:8 |  7:0 | Bit indices
        --   1. |   Q1  |   I1  |   Q0  |  I0   |   Q1  |   I1  |   Q0  |  I0  | Channels 0 & 1 enabled
        --   2. |   Q0' |   I0' |   Q0  |  I0   |   Q0' |   I0' |   Q0  |  I0  | Channel 0 only enabled
        --   3. |   Q1' |   I1' |   Q1  |  I1   |   Q1' |   I1' |   Q1  |  I1  | Channel 1 only enabled
        function pack_eight_bit_mode( sc : sample_controls_t;
                                      ss : sample_streams_t;
                                      d  : unsigned ) return unsigned is
            constant IQ_PAIR_LEN  : natural := ss(ss'low).data_i'length/2 + ss(ss'low).data_q'length/2;
            variable rv           : unsigned(d'range) := (others => '0');
        begin
            rv := d;
            for i in sc'range loop
                if( (sc(i).enable = '1') and (ss(i).data_v = '1') ) then
                    rv := unsigned(ss(i).data_q(11 downto 4)) &
                          unsigned(ss(i).data_i(11 downto 4)) &
                          rv(rv'high downto rv'low+IQ_PAIR_LEN);
                end if;
            end loop;
            return rv;
        end function;


        -- --------------------------------------------------------------------
        -- MIMO PACKER: STEP 1 of 3 (SC12Q11 Highly Packed Mode)
        -- --------------------------------------------------------------------
        -- This block receives samples as an array of sample streams, one
        -- element per channel. These streams need to be packed into a single,
        -- wide bus that is written to a FIFO and delivered to the host.
        -- For SC12Q11 (SC16_Q11_PACKED) format, each I and Q sample is 12 bits wide.
        --
        -- This packing scheme efficiently utilizes the available bandwidth
        -- for the SC12Q11 format, allowing for 8 complete IQ pairs every 3 cycles.
        --
        -- The packing process fills 3 cycles of a 64-bit buffer with 8 24b IQ pairs:
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
        function pack_sc12q11( sc : sample_controls_t;
                               ss : sample_streams_t;
                               d  : unsigned ) return unsigned is
            constant IQ_PAIR_LEN  : positive := 24;
            variable rv           : unsigned(d'range) := (others => '0');
        begin
            rv := d;
            for i in sc'range loop
                if( (sc(i).enable = '1') and (ss(i).data_v = '1') ) then
                    rv := unsigned(ss(i).data_q(11 downto 0)) &
                          unsigned(ss(i).data_i(11 downto 0)) &
                          rv(rv'high downto rv'low+IQ_PAIR_LEN);
                end if;
            end loop;
            return rv;
        end function;

        procedure handle_fifo_write(
            signal fifo_current : in fifo_fsm_t;
            signal meta_current : in meta_fsm_t;
            signal meta_en : in std_logic;
            signal eight_bit_mode_en : in std_logic;
            variable write_req : out std_logic;
            signal fifo_future : out fifo_fsm_t
        ) is
            variable in_sample_controls : sample_controls_t(0 to NUM_STREAMS-1);
            variable in_samples         : sample_streams_t(0 to NUM_STREAMS-1);
        begin
            in_sample_controls  := fifo_current.in_sample_controls_r;
            in_samples          := fifo_current.in_samples_r;

            if( ((meta_current.meta_written = '1') or (meta_en = '0')) ) then
                -- Check for valid data
                write_req := '0';
                for i in in_sample_controls'range loop
                    if( in_sample_controls(i).enable = '1' ) then
                        write_req := write_req or in_samples(i).data_v;
                    end if;
                end loop;

                -- Received valid data
                if( write_req = '1' ) then
                    if( fifo_current.samples_left = 0 ) then
                        fifo_future.samples_left <= NUM_STREAMS - count_enabled_channels(in_sample_controls);
                        fifo_future.fifo_write <= write_req;

                        if( eight_bit_mode_en = '1' ) then
                            fifo_future.fifo_write <= write_req and fifo_current.eight_bit_delay;
                            fifo_future.eight_bit_delay <= not fifo_current.eight_bit_delay;
                        end if;

                        if( highly_packed_mode_en = '1' ) then
                            fifo_future.write_cycle <= (fifo_current.write_cycle + 1) mod 4;
                            if fifo_current.write_cycle = 0 then
                                fifo_future.fifo_write <= '0';
                            end if;
                        end if;
                    else
                        -- MIMO PACKER: STEP 3 of 3
                        fifo_future.samples_left <= fifo_current.samples_left - 1;
                    end if;
                end if;
            else
                fifo_future.fifo_write <= '0';
            end if;
        end;

        variable write_req : std_logic := '0';

    begin

        fifo_future            <= fifo_current;

        fifo_future.fifo_clear <= '0';
        fifo_future.fifo_write <= '0';

        fifo_future.in_samples_r <= in_samples;
        fifo_future.in_sample_controls_r <= in_sample_controls;

        -- MIMO PACKER: STEP 1 of 3
        if( packet_en = '0' ) then
            fifo_future.fifo_data  <= pack(fifo_current.in_sample_controls_r,
                                           fifo_current.in_samples_r,
                                           fifo_current.fifo_data);

            if( highly_packed_mode_en = '1' ) then
                fifo_future.fifo_12b_buf <= pack_sc12q11(
                    sc => in_sample_controls,
                    ss => in_samples,
                    d => fifo_current.fifo_12b_buf
                );

                if fifo_current.write_cycle = 1 then
                    fifo_future.fifo_data <= fifo_current.fifo_12b_buf(
                        fifo_data'length-1 downto fifo_current.fifo_12b_buf'low);
                elsif fifo_current.write_cycle = 2 then
                    fifo_future.fifo_data <= fifo_current.fifo_12b_buf(
                        fifo_data'length-1+16 downto fifo_current.fifo_12b_buf'low+16);
                elsif fifo_current.write_cycle = 3 then
                    fifo_future.fifo_data <= fifo_current.fifo_12b_buf(
                        fifo_data'length-1+32 downto fifo_current.fifo_12b_buf'low+32);
                end if;
            end if;

            if( eight_bit_mode_en = '1' ) then
                fifo_future.fifo_data <= pack_eight_bit_mode(fifo_current.in_sample_controls_r,
                                                             fifo_current.in_samples_r,
                                                             fifo_current.fifo_data);
            end if;
        end if;

        case fifo_current.state is

            when CLEAR =>

                -- MIMO PACKER: STEP 2 of 3
                --   Compute "samples left" to fill up the fifo_data bus
                fifo_future.samples_left      <= NUM_STREAMS - count_enabled_channels(in_sample_controls);

                if( enable = '1' ) then
                    fifo_future.fifo_clear <= '0';
                    if( packet_en = '1' ) then
                       fifo_future.state      <= WRITE_PACKET_PAYLOAD;
                       fifo_future.samples_left <= NUM_STREAMS - 1;
                    else
                       fifo_future.state      <= WRITE_SAMPLES;
                    end if;
                else
                    fifo_future.fifo_clear <= '1';
                    fifo_future.eight_bit_delay <= '0';
                end if;

            when WRITE_PACKET_PAYLOAD =>

                if( meta_current.meta_written = '1' or (fifo_enough and packet_control.pkt_sop = '1' )) then
                    -- This code converts DWORD packet data into a fifo_data std_logic_vector
                    -- that is controlled by a generic.
                    if( packet_control.data_valid = '1' ) then
                        if( packet_control.pkt_eop = '1' and fifo_current.samples_left /= 0 ) then
                           -- End of packet asserted, however fifo_data is not full so
                           -- zero out the unset bits, commit what has been accumulated
                           fifo_future.samples_left <= NUM_STREAMS - 1;
                           fifo_future.fifo_write <= '1';

                           if (fifo_current.fifo_data'high > 31) then
                               fifo_future.fifo_data(fifo_future.fifo_data'high
                                           downto fifo_future.fifo_data'high - 31) <= (others => '0');
                           end if;
                           fifo_future.fifo_data(31 downto 0) <= unsigned(packet_control.data);
                        elsif( fifo_current.samples_left = 0 ) then
                           -- DWORDs perfectly filled fifo_data, commit fifo_data
                           fifo_future.samples_left <= NUM_STREAMS - 1;
                           fifo_future.fifo_write <= '1';

                           if (fifo_current.fifo_data'high > 31) then
                              fifo_future.fifo_data <= unsigned(packet_control.data) &
                                 fifo_current.fifo_data(fifo_current.fifo_data'high downto 32);
                           else
                              fifo_future.fifo_data <= unsigned(packet_control.data);
                           end if;
                        else
                           fifo_future.fifo_data(fifo_future.fifo_data'high
                                       downto fifo_future.fifo_data'high - 31) <=
                                             unsigned(packet_control.data);

                           if (fifo_current.fifo_data'high > 31) then
                               fifo_future.fifo_data(fifo_future.fifo_data'high - 32 downto 0) <=
                                        (others => '0');
                           end if;
                           fifo_future.samples_left <= fifo_current.samples_left - 1;
                        end if;

                    end if;
                end if;

            when WRITE_SAMPLES =>

                handle_fifo_write(
                    fifo_current       => fifo_current,
                    meta_current       => meta_current,
                    meta_en            => meta_en,
                    eight_bit_mode_en  => eight_bit_mode_en,
                    write_req          => write_req,
                    fifo_future        => fifo_future
                );

                if( fifo_full = '1' or ( meta_current.meta_written = '0' and meta_en = '1') ) then
                    fifo_future.fifo_write <= '0';
                    fifo_future.state      <= HOLDOFF;
                    fifo_future.write_cycle <= fifo_current.write_cycle;
                    fifo_future.fifo_12b_buf <= fifo_current.fifo_12b_buf;
                end if;

            when HOLDOFF =>
                fifo_future.write_cycle <= fifo_current.write_cycle;
                fifo_future.fifo_12b_buf <= fifo_current.fifo_12b_buf;
                if( fifo_enough ) then
                    fifo_future.state <= WRITE_SAMPLES;

                    handle_fifo_write(
                        fifo_current       => fifo_current,
                        meta_current       => meta_current,
                        meta_en            => meta_en,
                        eight_bit_mode_en  => eight_bit_mode_en,
                        write_req          => write_req,
                        fifo_future        => fifo_future
                    );

                end if;

            when others =>

                fifo_future.state <= CLEAR;

        end case;

        -- Abort?
        -- abort_active_i is registered (latch_usb_speed process); reading it
        -- here only steers fifo_future.state, which reaches fifo_write on
        -- the FOLLOWING clock through fifo_current -- the output assignment
        -- below stays a plain mirror of fifo_current.fifo_write, unchanged.
        -- ARMED (enable high, no epoch yet) holds the FIFO in clear too, so
        -- the first sample written belongs to the epoch, not to the interval
        -- before it.
        if( enable = '0' or abort_active_i = '1' or link_active_i = '0' ) then
            fifo_future.fifo_clear <= '1';
            fifo_future.fifo_write <= '0';
            fifo_future.state      <= CLEAR;
        end if;

        -- Output assignments
        fifo_clear <= fifo_current.fifo_clear;
        fifo_write <= fifo_current.fifo_write;
        fifo_data  <= std_logic_vector(fifo_current.fifo_data);

    end process;


    -- ------------------------------------------------------------------------
    -- OVERFLOW
    -- ------------------------------------------------------------------------

    -- Overflow detection
    detect_overflows : process( clock, reset )
    begin
        if( reset = '1' ) then
            overflow_detected <= '0';
        elsif( rising_edge( clock ) ) then
            overflow_detected <= '0';
            if( enable = '1' and in_samples(in_samples'low).data_v = '1' and
                fifo_full = '1' and fifo_current.fifo_clear = '0' ) then
                overflow_detected <= '1';
            end if;
        end if;
    end process;

    -- Count the number of times we overflow, but only if they are discontinuous
    -- meaning we have an overflow condition, a non-overflow condition, then
    -- another overflow condition counts as 2 overflows, but an overflow condition
    -- followed by N overflow conditions counts as a single overflow condition.
    count_overflows : process( clock, reset )
        variable prev_overflow : std_logic := '0';
    begin
        if( reset = '1' ) then
            prev_overflow  := '0';
            overflow_count <= (others =>'0');
        elsif( rising_edge( clock ) ) then
            if( prev_overflow = '0' and overflow_detected = '1' ) then
                overflow_count <= overflow_count + 1;
            end if;
            prev_overflow := overflow_detected;
        end if;
    end process;

    -- Active high assertion for overflow_duration when the overflow
    -- condition has been detected.  The LED will stay asserted
    -- if multiple overflows have occurred
    blink_overflow_led : process( clock, reset )
        variable downcount : natural range 0 to 2**overflow_duration'length-1;
    begin
        if( reset = '1' ) then
            downcount    := 0;
            overflow_led <= '0';
        elsif( rising_edge(clock) ) then
            -- Default to not being asserted
            overflow_led <= '0';

            -- Countdown so we can see what happened
            if( overflow_detected = '1' ) then
                downcount := to_integer(overflow_duration);
            elsif( downcount /= 0 ) then
                downcount := downcount - 1;
                overflow_led <= '1';
            end if;
        end if;
    end process;

end architecture;
