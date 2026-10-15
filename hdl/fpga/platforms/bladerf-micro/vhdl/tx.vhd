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
    use work.bladerf_p.all;
    use work.fifo_readwrite_p.all;

entity tx is
    generic (
        NUM_STREAMS          : natural := 2
    );
    port (
        sys_clock            : in    std_logic := '0';
        sys_reset            : in    std_logic := '0';
        fault_gpif_idle_sys  : in    std_logic := '0';
        tx_reset             : in    std_logic;
        tx_clock             : in    std_logic;
        tx_enable            : in    std_logic;

        meta_en              : in    std_logic := '0';
        timestamp_reset      : out   std_logic := '1';
        usb_speed            : in    std_logic;
        tx_underflow_led     : out   std_logic := '1';
        -- Starved-read counter, carried to the host (TX_UNDERFLOW_COUNT
        -- PIO). Was `open`, same defect as the RX side: the reader
        -- counted every time it had to transmit with an empty FIFO --
        -- a hole on the air -- and the number stayed inside the chip.
        tx_underflow_count   : out   unsigned(63 downto 0) := (others => '0');
        tx_timestamp         : in    unsigned(63 downto 0);

        -- TX retune fence controls are bundled/synchronized by the system
        -- controller. The defaults preserve existing non-event-driven
        -- images until the NIOS mailbox is connected at the core level.
        retune_gate_arm      : in std_logic := '0';
        retune_gate_arm_ack_toggle : in std_logic := '0';
        retune_gate_immediate : in std_logic := '0';
        retune_gate_target   : in unsigned(63 downto 0) := (others => '0');
        retune_gate_complete : in std_logic := '0';
        retune_gate_abort    : in std_logic := '0';
        retune_gate_fifo_flush_sys : in std_logic := '0';
        retune_gate_fifo_flush_release_sys : in std_logic := '0';
        retune_gate_status   : out std_logic_vector(31 downto 0) := (others => '0');
        fault_flush_status_sys : out std_logic_vector(5 downto 0) := (others => '0');
        fault_flush_aclr      : out std_logic := '0';
        fault_stop_pclk       : out std_logic := '0';

        -- Link status (host register RF_LINK_STATUS)
        link_start_toggle          : in    std_logic := '0';
        link_stop_toggle           : in    std_logic := '0';
        clear_fault_toggle         : in    std_logic := '0';
        usb_speed_mismatch         : out   std_logic := '0';
        link_active                : out   std_logic := '0';
        speed_latched               : out   std_logic := '0';
        protocol_start_violation   : out   std_logic := '0';
        link_epoch_counter         : out   unsigned(7 downto 0) := (others => '0');
        fault_sticky               : out   std_logic_vector(4 downto 0) := (others => '0');
        abort_active               : out   std_logic := '0';
        epoch_ack                  : out   std_logic := '0';
        epoch_valid                : out   std_logic := '0';

        -- Triggering
        trigger_arm          : in    std_logic;
        trigger_fire         : in    std_logic;
        trigger_master       : in    std_logic;
        trigger_line         : inout std_logic; -- this is not good, should be in/out/oe

        -- Packing modes
        eight_bit_mode_en      : in std_logic := '0';
        highly_packed_mode_en  : in std_logic := '0';

        -- Packet from host via FX3
        packet_en            : in    std_logic;
        packet_empty         : out   std_logic;
        packet_control       : out   packet_control_t;
        packet_ready         : in    std_logic;

        -- Samples from host via FX3
        sample_fifo_wclock   : in    std_logic;
        sample_fifo_wreq     : in    std_logic;
        sample_fifo_wdata    : in    std_logic_vector(TX_FIFO_T_DEFAULT.wdata'range);
        sample_fifo_wempty   : out   std_logic;
        sample_fifo_wfull    : out   std_logic;
        sample_fifo_wused    : out   std_logic_vector(TX_FIFO_T_DEFAULT.wused'range);

        -- Metadata from host via FX3
        meta_fifo_wclock     : in    std_logic;
        meta_fifo_wreq       : in    std_logic;
        meta_fifo_wdata      : in    std_logic_vector(META_FIFO_TX_T_DEFAULT.wdata'range);
        meta_fifo_wempty     : out   std_logic;
        meta_fifo_wfull      : out   std_logic;
        meta_fifo_wused      : out   std_logic_vector(META_FIFO_TX_T_DEFAULT.wused'range);

        -- Digital Loopback Interface
        loopback_enabled     : in    std_logic;
        loopback_fifo_wclock : out   std_logic;
        loopback_fifo_wreq   : out   std_logic;
        loopback_fifo_wdata  : out   std_logic_vector(LOOPBACK_FIFO_T_DEFAULT.wdata'range);
        loopback_fifo_wfull  : in    std_logic;
        loopback_fifo_wused  : in    std_logic_vector(LOOPBACK_FIFO_T_DEFAULT.wused'range);

        -- RFFE Interface
        dac_controls         : in    sample_controls_t(0 to NUM_STREAMS-1) := (others => SAMPLE_CONTROL_DISABLE);
        dac_streams          : out   sample_streams_t(0 to NUM_STREAMS-1)  := (others => ZERO_SAMPLE)
    );
end entity;

architecture arch of tx is

    signal sample_fifo                    : tx_fifo_t      := TX_FIFO_T_DEFAULT;
    signal meta_fifo                      : meta_fifo_tx_t := META_FIFO_TX_T_DEFAULT;

    signal trigger_arm_sync               : std_logic;
    signal trigger_line_sync              : std_logic;
    signal sample_fifo_rempty_untriggered : std_logic;

    signal sample_fifo_holdoff            : std_logic;
    signal sample_fifo_holdoff_i          : std_logic;
    signal loopback_fifo_near_full         : std_logic;
    signal loopback_fifo_source_data      : std_logic_vector(sample_fifo.rdata'range);
    signal loopback_fifo_source_valid     : std_logic;
    signal dac_streams_raw                : sample_streams_t(0 to NUM_STREAMS-1) := (others => ZERO_SAMPLE);
    signal tx_underflow_led_i              : std_logic := '0';
    signal tx_underflow_event              : std_logic := '0';
    signal retune_gate_state               : unsigned(1 downto 0);
    signal retune_gate_arm_ack_seen        : std_logic := '0';
    signal retune_gate_muted               : std_logic;
    signal retune_gate_muted_status         : std_logic;
    signal retune_gate_mute_event          : std_logic;
    signal retune_gate_resume_event        : std_logic;
    signal retune_gate_mute_timestamp      : unsigned(63 downto 0);
    signal retune_gate_muted_sample_count  : unsigned(63 downto 0);
    signal retune_gate_mute_seen           : std_logic := '0';
    signal retune_gate_resume_seen         : std_logic := '0';
    signal tx_fifo_flush_aclr              : std_logic;
    signal tx_fifo_write_block             : std_logic;
    signal tx_fifo_reader_block            : std_logic;
    signal tx_fifo_reader_block_tx         : std_logic;
    signal tx_fifo_flush_busy              : std_logic;
    signal tx_fifo_flush_done              : std_logic;
    signal tx_fifo_flush_error             : std_logic;
    signal tx_fifo_flush_empty_confirmed   : std_logic;
    signal tx_fifo_flush_release_complete  : std_logic;
    signal tx_fifo_flush_write_drain_ready : std_logic;

begin

    fault_flush_aclr <= tx_fifo_flush_aclr;

    set_timestamp_reset : process(tx_clock, tx_reset)
    begin
        if( tx_reset = '1' ) then
            timestamp_reset <= '1';
        elsif( rising_edge(tx_clock) ) then
            if( meta_en = '1' ) then
                timestamp_reset <= '0';
            else
                timestamp_reset <= '1';
            end if;
        end if;
    end process;

    U_tx_fifo_fault_flush : entity work.tx_fifo_fault_flush
        port map (
            sys_clock       => sys_clock,
            sys_reset       => sys_reset,
            tx_clock        => tx_clock,
            tx_reset        => tx_reset,
            write_clock     => sample_fifo_wclock,
            start           => retune_gate_fifo_flush_sys,
            release_request => retune_gate_fifo_flush_release_sys,
            gpif_idle       => fault_gpif_idle_sys,
            sample_wempty   => sample_fifo.wempty,
            meta_wempty     => meta_fifo.wempty,
            sample_rempty   => sample_fifo_rempty_untriggered,
            meta_rempty     => meta_fifo.rempty,
            fifo_aclr       => tx_fifo_flush_aclr,
            write_block     => tx_fifo_write_block,
            reader_block    => tx_fifo_reader_block,
            busy            => tx_fifo_flush_busy,
            done            => tx_fifo_flush_done,
            error           => tx_fifo_flush_error,
            empty_confirmed => tx_fifo_flush_empty_confirmed,
            release_complete => tx_fifo_flush_release_complete,
            write_drain_ready => tx_fifo_flush_write_drain_ready
        );

    -- busy is generated in sys_clock. Assert the TX reader barrier
    -- asynchronously, then release it only on tx_clock edges so it cannot
    -- form a timed cross-domain path into FIFO read control and address logic.
    U_tx_fifo_reader_block_sync : entity work.reset_synchronizer
        generic map (INPUT_LEVEL => '1', OUTPUT_LEVEL => '1')
        port map (
            clock => tx_clock,
            async => tx_reset or tx_fifo_reader_block,
            sync => tx_fifo_reader_block_tx
        );

    -- TX sample fifo. This private clear is scoped to TX; tx_reset remains
    -- the shared fabric reset and is never pulsed for this recovery operation.
    sample_fifo.aclr   <= tx_fifo_flush_aclr;
    sample_fifo.rclock <= tx_clock;
    U_tx_sample_fifo : entity work.tx_fifo
        generic map (
            LPM_NUMWORDS        => 2**(sample_fifo.wused'length)
        ) port map (
            aclr                => sample_fifo.aclr,

            wrclk               => sample_fifo_wclock,
            wrreq               => sample_fifo_wreq and not tx_fifo_write_block,
            data                => sample_fifo_wdata,
            wrempty             => sample_fifo_wempty,
            wrfull              => sample_fifo_wfull,
            wrusedw             => sample_fifo_wused,

            rdclk               => sample_fifo.rclock,
            rdreq               => sample_fifo.rreq,
            q                   => sample_fifo.rdata,
            rdempty             => sample_fifo_rempty_untriggered,
            rdfull              => sample_fifo.rfull,
            rdusedw             => sample_fifo.rused
        );

    -- TX metadata fifo
    meta_fifo.aclr   <= tx_fifo_flush_aclr;
    meta_fifo.rclock <= tx_clock;
    U_tx_meta_fifo : entity work.tx_meta_fifo
        generic map (
            LPM_NUMWORDS        => 2**(meta_fifo.wused'length)
        ) port map (
            aclr                => meta_fifo.aclr,

            wrclk               => meta_fifo_wclock,
            wrreq               => meta_fifo_wreq and not tx_fifo_write_block,
            data                => meta_fifo_wdata,
            wrempty             => meta_fifo_wempty,
            wrfull              => meta_fifo_wfull,
            wrusedw             => meta_fifo_wused,

            rdclk               => meta_fifo.rclock,
            rdreq               => meta_fifo.rreq,
            q                   => meta_fifo.rdata,
            rdempty             => meta_fifo.rempty,
            rdfull              => meta_fifo.rfull,
            rdusedw             => meta_fifo.rused
        );

    loopback_fifo_wclock <= tx_clock;

    loopback_proc : process( tx_clock )
    begin
        if( rising_edge(tx_clock) ) then
            -- fifo_reader may physically prefetch packed words before their
            -- scheduled output boundary. Use its logical-consumption sideband
            -- so digital loopback keeps the TX sample timeline.
            loopback_fifo_wdata <= loopback_fifo_source_data;
            loopback_fifo_wreq  <= loopback_fifo_source_valid and loopback_enabled;
        end if;
    end process;

    -- Stop reading TX samples before the loopback FIFO is full. The exact
    -- full flag feeds a wide combinational admission path into the TX sample
    -- FIFO's M10K read control; a 12-word reserve covers the registered
    -- holdoff response and the already-registered loopback write request.
    loopback_fifo_near_full <= '1' when
        unsigned(loopback_fifo_wused) >= to_unsigned(
            LOOPBACK_FIFO_LENGTH - 12, loopback_fifo_wused'length)
        else '0';
    sample_fifo_holdoff <= sample_fifo_holdoff_i;

    -- Persist sample fifo holdoff for a number of clocks, to give time for
    -- the FIFO to drain properly
    sample_fifo_holdoff_proc : process( tx_clock )
        variable backoff_downcount : natural range 0 to 3;
    begin
        if( rising_edge(tx_clock) ) then
            sample_fifo_holdoff_i <= '0';

            if( loopback_enabled = '1' ) then
                if( backoff_downcount /= 0 ) then
                    backoff_downcount       := backoff_downcount - 1;
                    sample_fifo_holdoff_i   <= '1';
                end if;

                if( loopback_fifo_near_full = '1' ) then
                    backoff_downcount       := 3;
                    sample_fifo_holdoff_i   <= '1';
                end if;
            end if;
        end if;
    end process;


    U_fifo_reader : entity work.fifo_reader
        generic map (
            NUM_STREAMS           => NUM_STREAMS,
            FIFO_READ_THROTTLE    => 0,
            FIFO_USEDW_WIDTH      => sample_fifo.rused'length,
            FIFO_DATA_WIDTH       => sample_fifo.rdata'length,
            META_FIFO_USEDW_WIDTH => meta_fifo.rused'length,
            META_FIFO_DATA_WIDTH  => meta_fifo.rdata'length
        )
        port map (
            clock               =>  tx_clock,
            reset               =>  tx_reset,
            enable              =>  tx_enable,
            fifo_flush          =>  tx_fifo_reader_block_tx,

            usb_speed           =>  usb_speed,
            meta_en             =>  meta_en,
            packet_en           =>  packet_en,
            timestamp           =>  tx_timestamp,

            link_start_toggle          =>  link_start_toggle,
            link_stop_toggle           =>  link_stop_toggle,
            clear_fault_toggle         =>  clear_fault_toggle,
            usb_speed_mismatch         =>  usb_speed_mismatch,
            link_active                =>  link_active,
            speed_latched              =>  speed_latched,
            protocol_start_violation   =>  protocol_start_violation,
            link_epoch_counter         =>  link_epoch_counter,
            fault_sticky               =>  fault_sticky,
            abort_active               =>  abort_active,
            epoch_ack                  =>  epoch_ack,
            epoch_valid                =>  epoch_valid,

            eight_bit_mode_en   =>  eight_bit_mode_en,
            highly_packed_mode_en =>  highly_packed_mode_en,

            fifo_empty          =>  sample_fifo.rempty,
            fifo_usedw          =>  sample_fifo.rused,
            fifo_data           =>  sample_fifo.rdata,
            fifo_read           =>  sample_fifo.rreq,
            fifo_holdoff        =>  sample_fifo_holdoff,

            packet_control      =>  packet_control,
            packet_empty        =>  packet_empty,
            packet_ready        =>  packet_ready,

            meta_fifo_empty     =>  meta_fifo.rempty,
            meta_fifo_usedw     =>  meta_fifo.rused,
            meta_fifo_data      =>  meta_fifo.rdata,
            meta_fifo_read      =>  meta_fifo.rreq,

            in_sample_controls  =>  dac_controls,
            out_samples         =>  dac_streams_raw,

            underflow_led       =>  tx_underflow_led_i,
            underflow_count     =>  tx_underflow_count,
            underflow_event     =>  tx_underflow_event,
            underflow_duration  =>  x"ffff",
            underflow_suppress  =>  retune_gate_muted,
            loopback_data       =>  loopback_fifo_source_data,
            loopback_data_valid =>  loopback_fifo_source_valid
        );

    U_tx_retune_gate : entity work.tx_retune_gate
        generic map (
            NUM_STREAMS => NUM_STREAMS
        )
        port map (
            clock                => tx_clock,
            reset                => tx_reset,
            timestamp            => tx_timestamp,
            arm                  => retune_gate_arm and not tx_fifo_reader_block_tx,
            arm_immediate        => retune_gate_immediate,
            target_timestamp     => retune_gate_target,
            complete             => retune_gate_complete and not tx_fifo_reader_block_tx,
            -- A one-clock underflow event feeds the gate directly; the
            -- stretched underflow LED is only for front-panel visibility.
            abort                => retune_gate_abort,
            underflow            => tx_underflow_event,
            in_samples           => dac_streams_raw,
            out_samples          => dac_streams,
            state                => retune_gate_state,
            muted                => retune_gate_muted,
            mute_start_event     => retune_gate_mute_event,
            resume_event         => retune_gate_resume_event,
            mute_start_timestamp => retune_gate_mute_timestamp,
            muted_sample_count  => retune_gate_muted_sample_count
        );

    tx_underflow_led <= tx_underflow_led_i;

    process(tx_clock, tx_reset)
    begin
        if tx_reset = '1' then
            retune_gate_mute_seen <= '0';
            retune_gate_resume_seen <= '0';
            retune_gate_arm_ack_seen <= '0';
        elsif rising_edge(tx_clock) then
            if retune_gate_arm = '1' then
                retune_gate_mute_seen <= '0';
                retune_gate_resume_seen <= '0';
                retune_gate_arm_ack_seen <= retune_gate_arm_ack_toggle;
            else
                -- The gate controller re-baselines its retained NIOS ARM
                -- toggle after TX reset. Keep this status level aligned even
                -- before the next ARM pulse, so a fresh 1->0 command is still
                -- observable to the host's before/after toggle comparison.
                retune_gate_arm_ack_seen <= retune_gate_arm_ack_toggle;
                if retune_gate_mute_event = '1' then
                    retune_gate_mute_seen <= '1';
                end if;
                if retune_gate_resume_event = '1' then
                    retune_gate_resume_seen <= '1';
                end if;
            end if;
        end if;
    end process;

    -- The telemetry bit follows the registered state. The DAC mute itself
    -- remains combinational in tx_retune_gate for exact timestamp alignment;
    -- exporting that live comparator result here made the CDC status bus
    -- timing-critical and is unnecessary for host state polling.
    retune_gate_muted_status <= '1' when retune_gate_state = "10" or
                                         retune_gate_state = "11" else '0';

    -- Only TX-clock-owned gate state crosses through the status handshake.
    -- Fault-flush state is exported separately from sys_clock by the core,
    -- so a stopped AD9361 clock cannot freeze its host-visible status.
    retune_gate_status <= (31 downto 6 => '0') & retune_gate_arm_ack_seen &
                          retune_gate_resume_seen &
                          retune_gate_mute_seen & retune_gate_muted_status &
                          std_logic_vector(retune_gate_state);

    fault_flush_status_sys <= tx_fifo_flush_write_drain_ready &
                              tx_fifo_flush_release_complete &
                              tx_fifo_flush_empty_confirmed &
                              tx_fifo_flush_error & tx_fifo_flush_done &
                              tx_fifo_flush_busy;
    fault_stop_pclk <= tx_fifo_write_block;

    txtrig : entity work.trigger(async)
        generic map (
            DEFAULT_OUTPUT  => '1'
        )
        port map (
            armed           => trigger_arm_sync,               -- in  sl
            fired           => trigger_fire,                   -- in  sl
            master          => trigger_master,                 -- in  sl
            trigger_in      => trigger_line_sync,              -- in  sl
            trigger_out     => trigger_line,                   -- out sl
            signal_in       => sample_fifo_rempty_untriggered, -- in  sl
            signal_out      => sample_fifo.rempty              -- out sl
        );


    U_reset_sync_txtrig_arm : entity work.reset_synchronizer
        generic map (
            INPUT_LEVEL     =>  '0',
            OUTPUT_LEVEL    =>  '0'
        )
        port map (
            clock           =>  tx_clock,
            async           =>  trigger_arm,
            sync            =>  trigger_arm_sync
        );

    U_sync_txtrig_trigger : entity work.synchronizer
        generic map (
            RESET_LEVEL =>  '0'
        )
        port map (
            reset       =>  tx_reset,
            clock       =>  tx_clock,
            async       =>  trigger_line,
            sync        =>  trigger_line_sync
        );

end architecture;
