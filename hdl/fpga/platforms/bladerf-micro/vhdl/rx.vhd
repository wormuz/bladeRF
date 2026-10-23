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

entity rx is
    generic (
        NUM_STREAMS          : natural := 2;
        -- 2x2 spatial FIR between adc_streams and rx_mux
        -- (doc/development/rx-spatial-mimo-fir-design.md). Off: stock chain.
        ENABLE_SPATIAL_FIR   : boolean := false;
        -- log2 of the sample FIFO depth in write words; 0 keeps the full FIFO.
        RX_SAMPLE_FIFO_LOG2  : natural := 0
    );
    port (
        rx_reset               : in    std_logic;
        rx_clock               : in    std_logic;
        rx_enable              : in    std_logic;

        meta_en                : in    std_logic := '0';
        timestamp_reset        : out   std_logic := '1';
        usb_speed              : in    std_logic;
        rx_mux_sel             : in    unsigned;
        rx_overflow_led        : out   std_logic := '1';
        -- Dropped-sample counter, carried to the host (RX_OVERFLOW_COUNT
        -- PIO). Was `open` here: the fabric counted every sample the
        -- writer had to discard and the number never left the chip, so
        -- the only evidence of a drop was an LED nobody polls. The host
        -- flag it did have (BLADERF_META_STATUS_OVERRUN) is computed on
        -- the host from USB queue state, not from this counter, and
        -- therefore cannot see a drop the fabric absorbed on its own.
        rx_overflow_count      : out   unsigned(63 downto 0) := (others => '0');
        rx_timestamp           : in    unsigned(63 downto 0);

        -- Link status (host register RF_LINK_STATUS)
        link_start_toggle          : in    std_logic := '0';
        link_stop_toggle           : in    std_logic := '0';
        clear_fault_toggle         : in    std_logic := '0';
        usb_speed_mismatch         : out   std_logic := '0';
        link_active                : out   std_logic := '0';
        speed_latched               : out   std_logic := '0';
        protocol_start_violation   : out   std_logic := '0';
        link_epoch_counter         : out   unsigned(7 downto 0) := (others => '0');
        fault_sticky               : out   std_logic_vector(5 downto 0) := (others => '0');
        fault_context              : out   std_logic_vector(24 downto 0) := (others => '0');
        gpif_diag                  : in    std_logic_vector(2 downto 0) := (others => '0');
        abort_active               : out   std_logic := '0';
        epoch_ack                  : out   std_logic := '0';
        epoch_valid                : out   std_logic := '0';

        -- ADR-0207 BLADE_RF_EVENT_DRIVEN_RF_STATE_001 §6: RX data-plane
        -- epoch gate. Separate from the link-epoch signals above (those
        -- gate USB-speed/whole-stream start-stop, this gates data validity
        -- across a single RX retune -- see rx_epoch_gate.vhd header).
        rx_epoch_arm            : in    std_logic := '0';
        rx_epoch_complete       : in    std_logic := '0';
        rx_epoch_abort          : in    std_logic := '0';
        rx_epoch_id_in          : in    unsigned(7 downto 0) := (others => '0');
        rx_epoch_meta_enable    : in    std_logic := '0';
        rx_epoch_id_out         : out   unsigned(7 downto 0) := (others => '0');
        rx_epoch_state          : out   unsigned(3 downto 0) := (others => '0');
        rx_epoch_discard_active : out   std_logic := '0';
        rx_epoch_start_event    : out   std_logic := '0';
        rx_epoch_first_valid_ts : out   unsigned(63 downto 0) := (others => '0');

        -- Spatial FIR bank control (rx_clock, from rx_spatial_fir_controller)
        -- and status. With ENABLE_SPATIAL_FIR false the status reads as a
        -- permanent identity bypass.
        sfir_coef_wr_en         : in    std_logic := '0';
        sfir_coef_wr_idx        : in    unsigned(4 downto 0) := (others => '0');
        sfir_coef_wr_data       : in    std_logic_vector(31 downto 0) := (others => '0');
        sfir_coef_commit        : in    std_logic := '0';
        sfir_coef_commit_tag    : in    unsigned(7 downto 0) := (others => '0');
        sfir_force_identity     : in    std_logic := '0';
        sfir_active_tag         : out   unsigned(7 downto 0) := (others => '0');
        sfir_bypass             : out   std_logic := '1';
        sfir_active_identity    : out   std_logic := '1';
        sfir_both_lanes         : out   std_logic := '0';
        sfir_sat_sticky         : out   std_logic_vector(1 downto 0) := (others => '0');

        -- Triggering
        trigger_arm            : in    std_logic;
        trigger_fire           : in    std_logic;
        trigger_master         : in    std_logic;
        trigger_line           : inout std_logic; -- this is not good, should be in/out/oe
        trigger_signal_sync_tb : out   std_logic;

        -- 8-bit mode
        eight_bit_mode_en      : in std_logic := '0';
        highly_packed_mode_en  : in std_logic := '0';

        -- Packet to host via FX3
        packet_en              : in    std_logic;
        packet_control         : in    packet_control_t;
        packet_ready           : out   std_logic;

        -- Samples to host via FX3
        sample_fifo_rclock     : in    std_logic;
        sample_fifo_raclr      : in    std_logic;
        sample_fifo_rreq       : in    std_logic;
        sample_fifo_rdata      : out   std_logic_vector(RX_FIFO_T_DEFAULT.rdata'range);
        sample_fifo_rempty     : out   std_logic;
        sample_fifo_rfull      : out   std_logic;
        sample_fifo_rused      : out   std_logic_vector(RX_FIFO_T_DEFAULT.rused'range);

        -- Mini expansion signals
        mini_exp               : in    std_logic_vector(1 downto 0);

        -- Metadata to host via FX3
        meta_fifo_rclock       : in    std_logic;
        meta_fifo_raclr        : in    std_logic;
        meta_fifo_rreq         : in    std_logic;
        meta_fifo_rdata        : out   std_logic_vector(META_FIFO_RX_T_DEFAULT.rdata'range);
        meta_fifo_rempty       : out   std_logic;
        meta_fifo_rfull        : out   std_logic;
        meta_fifo_rused        : out   std_logic_vector(META_FIFO_RX_T_DEFAULT.rused'range);

        -- Digital Loopback Interface
        loopback_fifo_wenabled : out   std_logic;
        loopback_fifo_wreset   : in    std_logic;
        loopback_fifo_wclock   : in    std_logic;
        loopback_fifo_wdata    : in    std_logic_vector(LOOPBACK_FIFO_T_DEFAULT.wdata'range);
        loopback_fifo_wreq     : in    std_logic;
        loopback_fifo_wfull    : out   std_logic;
        loopback_fifo_wused    : out   std_logic_vector(LOOPBACK_FIFO_T_DEFAULT.wused'range);

        -- RFFE Interface
        adc_controls           : in    sample_controls_t(0 to NUM_STREAMS-1) := (others => SAMPLE_CONTROL_DISABLE);
        adc_streams            : in    sample_streams_t(0 to NUM_STREAMS-1)  := (others => ZERO_SAMPLE)
    );
end entity;

architecture arch of rx is

    signal rx_packet : packet_control_t;

    -- Can be set from libbladeRF using bladerf_set_rx_mux()
    type rx_mux_mode_t is (
        RX_MUX_NORMAL,
        RX_MUX_12BIT_COUNTER,
        RX_MUX_32BIT_COUNTER,
        RX_MUX_ENTROPY,
        RX_MUX_DIGITAL_LOOPBACK
    );

    signal rx_mux_mode              : rx_mux_mode_t       := RX_MUX_NORMAL;

    signal sample_fifo              : rx_fifo_t           := RX_FIFO_T_DEFAULT;
    signal loopback_fifo            : loopback_fifo_t     := LOOPBACK_FIFO_T_DEFAULT;
    signal meta_fifo                : meta_fifo_rx_t      := META_FIFO_RX_T_DEFAULT;

    signal loopback_streams         : sample_streams_t(adc_streams'range) := (others => ZERO_SAMPLE);
    signal loopback_enabled         : std_logic           := '0';
    signal loopback_fifo_wenabled_i : std_logic           := '0';

    signal rx_gen_mode              : std_logic;
    signal rx_gen_i                 : signed(15 downto 0);
    signal rx_gen_q                 : signed(15 downto 0);
    signal rx_gen_valid             : std_logic;

    signal mux_streams              : sample_streams_t(adc_streams'range) := (others => ZERO_SAMPLE);

    -- Spatial FIR output and the rx_mux sidebands that carry its timestamp
    -- and delayed COMPLETE to the gate (design 1.1, 1.4, 3). The FIR carries
    -- ts_in(n-2) with each output sample; mux_ts_from_fir selects it only
    -- for samples that came through the NORMAL branch. Every other mux mode
    -- gives the gate the live rx_timestamp and rx_epoch_complete exactly as
    -- the stock chain does (a registered copy would lag the live counter by
    -- one rx_clock, i.e. half a sample).
    signal fir_streams              : sample_streams_t(adc_streams'range) := (others => ZERO_SAMPLE);
    signal fir_timestamp            : unsigned(63 downto 0) := (others => '0');
    signal fir_epoch_complete       : std_logic := '0';
    signal mux_timestamp            : unsigned(63 downto 0) := (others => '0');
    signal mux_epoch_complete       : std_logic := '0';
    signal mux_ts_from_fir          : std_logic := '0';
    signal gate_timestamp           : unsigned(63 downto 0);
    signal gate_epoch_complete      : std_logic;

    -- ADR-0207 §6.2: rx_epoch_gate sits between the mux output and
    -- fifo_writer. ARM fences new samples, but a sample already registered
    -- in the gate/writer pipeline may finish transport under its old epoch
    -- tag. libbladeRF rejects it after the new epoch is certified. Fenced
    -- states emit zero-IQ keepalives to preserve USB stream liveness.
    signal gated_controls           : sample_controls_t(adc_streams'range) := (others => SAMPLE_CONTROL_DISABLE);
    signal gated_streams            : sample_streams_t(adc_streams'range)  := (others => ZERO_SAMPLE);
    signal gated_timestamp          : unsigned(63 downto 0) := (others => '0');
    signal rx_epoch_id_local        : unsigned(7 downto 0) := (others => '0');
    signal rx_epoch_discard_active_local : std_logic := '0';
    signal rx_epoch_fault_local          : std_logic := '0';

    signal trigger_signal_out       : std_logic;
    signal trigger_signal_out_sync  : std_logic;
    signal trigger_arm_sync         : std_logic;

begin

    rx_mux_mode            <= rx_mux_mode_t'val(to_integer(rx_mux_sel));
    loopback_fifo_wenabled <= loopback_fifo_wenabled_i;
    rx_epoch_id_out        <= rx_epoch_id_local;
    rx_epoch_discard_active <= rx_epoch_discard_active_local;

    set_timestamp_reset : process(rx_clock, rx_reset)
    begin
        if( rx_reset = '1' ) then
            timestamp_reset <= '1';
        elsif( rising_edge(rx_clock) ) then
            if( meta_en = '1' ) then
                timestamp_reset <= '0';
            else
                timestamp_reset <= '1';
            end if;
        end if;
    end process;


    -- RX sample FIFO
    sample_fifo.aclr   <= sample_fifo_raclr;
    sample_fifo.wclock <= rx_clock;
    gen_rx_sample_fifo_full : if RX_SAMPLE_FIFO_LOG2 = 0 generate
        U_rx_sample_fifo : entity work.rx_fifo
            generic map (
                LPM_NUMWORDS        => 2**(sample_fifo.wused'length)
            ) port map (
                aclr                => sample_fifo.aclr,

                wrclk               => sample_fifo.wclock,
                wrreq               => sample_fifo.wreq,
                data                => sample_fifo.wdata,
                wrempty             => sample_fifo.wempty,
                wrfull              => sample_fifo.wfull,
                wrusedw             => sample_fifo.wused,

                rdclk               => sample_fifo_rclock,
                rdreq               => sample_fifo_rreq,
                q                   => sample_fifo_rdata,
                rdempty             => sample_fifo_rempty,
                rdfull              => sample_fifo_rfull,
                rdusedw             => sample_fifo_rused
            );
    end generate;

    -- 2**RX_SAMPLE_FIFO_LOG2 write words instead of the full 8192, as for the
    -- TX FIFO (tx.vhd). The used counts are the true counts, zero-extended to
    -- the full widths; fifo_writer (FIFO_DEPTH_LOG2) and fx3_gpif
    -- (RX_FIFO_WORDS) get the real depth.
    gen_rx_sample_fifo_small : if RX_SAMPLE_FIFO_LOG2 /= 0 generate
        signal wused_small : std_logic_vector(RX_SAMPLE_FIFO_LOG2-1 downto 0);
        -- The read side is narrower words, so its count is that many bits
        -- longer than the write side, as for the full FIFO.
        signal rused_small : std_logic_vector(
            RX_SAMPLE_FIFO_LOG2 - 1 +
            (sample_fifo_rused'length - sample_fifo.wused'length) downto 0);
    begin
        assert RX_SAMPLE_FIFO_LOG2 < sample_fifo.wused'length
            report "RX_SAMPLE_FIFO_LOG2 must be below the full FIFO depth"
            severity failure;

        U_rx_sample_fifo : entity work.rx_fifo
            generic map (
                LPM_NUMWORDS        => 2**RX_SAMPLE_FIFO_LOG2
            ) port map (
                aclr                => sample_fifo.aclr,

                wrclk               => sample_fifo.wclock,
                wrreq               => sample_fifo.wreq,
                data                => sample_fifo.wdata,
                wrempty             => sample_fifo.wempty,
                wrfull              => sample_fifo.wfull,
                wrusedw             => wused_small,

                rdclk               => sample_fifo_rclock,
                rdreq               => sample_fifo_rreq,
                q                   => sample_fifo_rdata,
                rdempty             => sample_fifo_rempty,
                rdfull              => sample_fifo_rfull,
                rdusedw             => rused_small
            );

        sample_fifo.wused <= std_logic_vector(resize(unsigned(wused_small),
                                                     sample_fifo.wused'length));
        sample_fifo_rused <= std_logic_vector(resize(unsigned(rused_small),
                                                     sample_fifo_rused'length));
    end generate;


    -- RX meta FIFO
    meta_fifo.aclr   <= meta_fifo_raclr;
    meta_fifo.wclock <= rx_clock;
    U_rx_meta_fifo : entity work.rx_meta_fifo
        generic map (
            LPM_NUMWORDS        => 2**(meta_fifo.wused'length)
        ) port map (
            aclr                => meta_fifo.aclr,

            wrclk               => meta_fifo.wclock,
            wrreq               => meta_fifo.wreq,
            data                => meta_fifo.wdata,
            wrempty             => meta_fifo.wempty,
            wrfull              => meta_fifo.wfull,
            wrusedw             => meta_fifo.wused,

            rdclk               => meta_fifo_rclock,
            rdreq               => meta_fifo_rreq,
            q                   => meta_fifo_rdata,
            rdempty             => meta_fifo_rempty,
            rdfull              => meta_fifo_rfull,
            rdusedw             => meta_fifo_rused
        );


    -- RX loopback FIFO
    loopback_fifo.aclr   <= '1' when ( (loopback_fifo_wreset = '1') or (loopback_fifo_wenabled_i = '0') ) else '0';
    loopback_fifo.rclock <= rx_clock;

    U_rx_loopback_fifo : entity work.lb_fifo
        generic map (
            LPM_NUMWORDS        => 2**(loopback_fifo.rused'length)
        )
        port map (
            aclr                => loopback_fifo.aclr,

            wrclk               => loopback_fifo_wclock,
            wrreq               => loopback_fifo_wreq,
            data                => loopback_fifo_wdata,
            wrempty             => open,
            wrfull              => loopback_fifo_wfull,
            wrusedw             => loopback_fifo_wused,

            rdclk               => loopback_fifo.rclock,
            rdreq               => loopback_fifo.rreq,
            q                   => loopback_fifo.rdata,
            rdempty             => loopback_fifo.rempty,
            rdfull              => loopback_fifo.rfull,
            rdusedw             => loopback_fifo.rused
        );


    -- 2x2 spatial FIR (rx_spatial_fir.vhd) on the raw ADC lanes, in front of
    -- rx_mux, so the loopback and counter sources stay out of it. The block
    -- registers both lanes at its S0 delay line with no combinational fan-in;
    -- adc_streams keeps one reader here as before (the mux NORMAL branch now
    -- reads fir_streams instead).
    gen_spatial_fir : if ENABLE_SPATIAL_FIR generate
        assert NUM_STREAMS = 2
            report "rx_spatial_fir needs exactly two RX streams"
            severity failure;

        U_rx_spatial_fir : entity work.rx_spatial_fir
            port map (
                clock               => rx_clock,
                reset               => rx_reset,
                in_sample_controls  => adc_controls,
                in_samples          => adc_streams,
                rx_timestamp        => rx_timestamp,
                epoch_arm           => rx_epoch_arm,
                epoch_complete      => rx_epoch_complete,
                epoch_abort         => rx_epoch_abort,
                coef_wr_en          => sfir_coef_wr_en,
                coef_wr_idx         => sfir_coef_wr_idx,
                coef_wr_data        => sfir_coef_wr_data,
                coef_commit         => sfir_coef_commit,
                coef_commit_tag     => sfir_coef_commit_tag,
                force_identity      => sfir_force_identity,
                out_samples         => fir_streams,
                out_timestamp       => fir_timestamp,
                out_epoch_complete  => fir_epoch_complete,
                out_support_clean   => open,
                out_active_tag      => sfir_active_tag,
                out_bypass          => sfir_bypass,
                out_active_identity => sfir_active_identity,
                out_both_lanes      => sfir_both_lanes,
                out_sat_sticky      => sfir_sat_sticky
            );
    end generate;

    gen_no_spatial_fir : if not ENABLE_SPATIAL_FIR generate
        fir_streams          <= adc_streams;
        fir_timestamp        <= rx_timestamp;
        fir_epoch_complete   <= rx_epoch_complete;
        sfir_active_tag      <= (others => '0');
        sfir_bypass          <= '1';
        sfir_active_identity <= '1';
        sfir_both_lanes      <= '0';
        sfir_sat_sticky      <= (others => '0');
    end generate;

    -- Gate inputs: FIR-carried sidebands for NORMAL samples, live stock
    -- signals otherwise (see mux_ts_from_fir). Without the FIR the select is
    -- constant '0' and the gate wiring is the stock one.
    gate_timestamp      <= mux_timestamp when mux_ts_from_fir = '1' else rx_timestamp;
    gate_epoch_complete <= mux_epoch_complete when mux_ts_from_fir = '1' else rx_epoch_complete;

    -- ADR-0207 §6.2: gate between the mux and the sample bridge. It suppresses
    -- new IQ on ARM; a sample already registered before ARM can still pass
    -- through fifo_writer/rx_fifo with the old epoch identity. Host epoch
    -- filtering drops that in-flight data. During a normal transition fence,
    -- zero-IQ keepalives retain transport liveness. A terminal gate fault is
    -- separately wired to fifo_writer so it stops appending keepalives to
    -- any packet whose META header was emitted before the fault.
    U_rx_epoch_gate : entity work.rx_epoch_gate
        generic map (
            NUM_STREAMS             => NUM_STREAMS
        )
        port map (
            clock                   => rx_clock,
            reset                   => rx_reset,

            in_sample_controls      => adc_controls,
            in_samples              => mux_streams,
            rx_timestamp            => gate_timestamp,

            epoch_arm               => rx_epoch_arm,
            epoch_complete          => gate_epoch_complete,
            epoch_abort             => rx_epoch_abort,
            epoch_id_in             => rx_epoch_id_in,

            out_sample_controls     => gated_controls,
            out_samples             => gated_streams,
            out_timestamp           => gated_timestamp,

            out_epoch_id            => rx_epoch_id_local,
            out_state               => rx_epoch_state,
            out_discard_active      => rx_epoch_discard_active_local,
            out_fault               => rx_epoch_fault_local,
            epoch_start_event       => rx_epoch_start_event,
            first_valid_timestamp   => rx_epoch_first_valid_ts
        );


    -- Sample bridge
    U_fifo_writer : entity work.fifo_writer
        generic map (
            NUM_STREAMS           => NUM_STREAMS,
            FIFO_USEDW_WIDTH      => sample_fifo.wused'length,
            FIFO_DEPTH_LOG2       => RX_SAMPLE_FIFO_LOG2,
            FIFO_DATA_WIDTH       => sample_fifo.wdata'length,
            META_FIFO_USEDW_WIDTH => meta_fifo.wused'length,
            META_FIFO_DATA_WIDTH  => meta_fifo.wdata'length
        )
        port map (
            clock               =>  rx_clock,
            reset               =>  rx_reset,
            enable              =>  rx_enable,

            usb_speed           =>  usb_speed,
            meta_en             =>  meta_en,
            packet_en           =>  packet_en,
            timestamp           =>  gated_timestamp,
            mini_exp            =>  mini_exp,
            rx_epoch_meta_enable => rx_epoch_meta_enable,
            rx_epoch_id          => rx_epoch_id_local,
            rx_epoch_discard_active => rx_epoch_discard_active_local,
            rx_epoch_gate_fault     => rx_epoch_fault_local,

            link_start_toggle          =>  link_start_toggle,
            link_stop_toggle           =>  link_stop_toggle,
            clear_fault_toggle         =>  clear_fault_toggle,
            usb_speed_mismatch         =>  usb_speed_mismatch,
            link_active                =>  link_active,
            speed_latched              =>  speed_latched,
            protocol_start_violation   =>  protocol_start_violation,
            link_epoch_counter         =>  link_epoch_counter,
            fault_sticky               =>  fault_sticky,
            fault_context              =>  fault_context,
            gpif_diag                  =>  gpif_diag,
            abort_active               =>  abort_active,
            epoch_ack                  =>  epoch_ack,
            epoch_valid                =>  epoch_valid,

            fifo_full           =>  sample_fifo.wfull,
            fifo_usedw          =>  sample_fifo.wused,
            fifo_data           =>  sample_fifo.wdata,
            fifo_write          =>  sample_fifo.wreq,

            packet_control      =>  packet_control,
            packet_ready        =>  packet_ready,

            eight_bit_mode_en   => eight_bit_mode_en,
            highly_packed_mode_en => highly_packed_mode_en,

            meta_fifo_full      =>  meta_fifo.wfull,
            meta_fifo_usedw     =>  meta_fifo.wused,
            meta_fifo_data      =>  meta_fifo.wdata,
            meta_fifo_write     =>  meta_fifo.wreq,

            in_sample_controls  =>  gated_controls,
            in_samples          =>  gated_streams,

            overflow_led        =>  rx_overflow_led,
            overflow_count      =>  rx_overflow_count,
            overflow_duration   =>  x"ffff"
        );


    loopback_fifo_control : process( rx_reset, loopback_fifo.rclock )
        variable offset     : natural range 0 to loopback_fifo.rdata'length;
        variable remaining  : natural range 0 to loopback_fifo.rdata'length/16;
        variable loopdata   : std_logic_vector(loopback_fifo.rdata'range);
    begin
        if( rx_reset = '1' ) then
            loopback_enabled    <= '0';
            loopback_fifo.rreq  <= '0';
            loopback_streams    <= (others => ZERO_SAMPLE);
            remaining           := 0;
        elsif( rising_edge(loopback_fifo.rclock) ) then

            -- Is loopback enabled?
            -- Use the local registered enable for the loopback datapath.
            -- The synchronized module enable also fans out across the RX
            -- datapath; routing it through the packed-FIFO unpack/enable
            -- logic was the fitted critical path to loopback_streams.data_q.
            -- loopback_enabled is updated from rx_enable above, so gate
            -- sample-valid with rx_enable below to keep disable immediate
            -- while allowing data registers to use this local copy.
            if( rx_mux_mode = RX_MUX_DIGITAL_LOOPBACK and
                loopback_enabled = '1' ) then
                loopback_enabled    <= '1';
            else
                loopback_enabled    <= '0';
                loopback_fifo.rreq  <= '0';
                remaining           := 0;
            end if;

            -- Clear data valids
            for i in loopback_streams'range loop
                loopback_streams(i).data_v <= '0';
            end loop;

            -- Handle loopback FIFO
            if( loopback_fifo.rreq = '1' ) then
                -- We have fresh data!
                loopback_fifo.rreq <= '0';
                loopdata    := loopback_fifo.rdata;
                if( eight_bit_mode_en = '0' ) then
                    remaining := loopback_fifo.rdata'length/32;
                else
                    remaining := loopback_fifo.rdata'length/16;
                end if;
            elsif( remaining = 0 and sample_fifo.wfull = '0' and loopback_fifo.rempty = '0' ) then
                -- Read more from the FIFO if we can
                loopback_fifo.rreq <= '1';
            end if;

            -- Do the loopback
            -- Compute the first unread sample position once. Recomputing it
            -- from `remaining` for every channel made an earlier channel's
            -- enable feed a decrement, a stride multiply, and this channel's
            -- dynamic bit select in the same cycle. Advance the bit offset
            -- only when a sample is actually emitted; this preserves the
            -- FIFO ordering and removes the remaining-count arithmetic from
            -- the next channel's offset calculation.
            if( eight_bit_mode_en = '0' ) then
                offset := loopdata'length - (remaining*32);
            else
                offset := loopdata'length - (remaining*16);
            end if;

            for i in loopback_streams'range loop
                if( adc_controls(i).enable = '1' and remaining > 0 ) then
                    if( eight_bit_mode_en = '0' ) then
                        loopback_streams(i).data_i <=
                            resize(signed(loopdata(offset+11 downto offset+0)),
                                    loopback_streams(i).data_i'length);

                        loopback_streams(i).data_q <=
                            resize(signed(loopdata(offset+27 downto offset+16)),
                                    loopback_streams(i).data_q'length);
                    else
                        loopback_streams(i).data_i <=
                            shift_left(resize(signed(loopdata(offset+7 downto offset+0)),
                                    loopback_streams(i).data_i'length), 4);

                        loopback_streams(i).data_q <=
                            shift_left(resize(signed(loopdata(offset+15 downto offset+8)),
                                    loopback_streams(i).data_q'length), 4);
                    end if;

                    loopback_streams(i).data_v <= rx_enable;

                    -- Advance to the next unread sample in the packed FIFO
                    -- word. The previous implementation recomputed this
                    -- offset from the decremented remaining count on every
                    -- iteration.
                    if( remaining > 0 ) then
                        if( eight_bit_mode_en = '0' ) then
                            offset := offset + 32;
                        else
                            offset := offset + 16;
                        end if;
                        remaining := remaining - 1;
                    end if;
                end if;
            end loop;
        end if;
    end process;


    U_rx_siggen : entity work.signal_generator
        port map (
            clock           =>  rx_clock,
            reset           =>  rx_reset,
            enable          =>  rx_enable,

            mode            =>  rx_gen_mode,
            eightbit_en     => eight_bit_mode_en,

            sample_i        =>  rx_gen_i,
            sample_q        =>  rx_gen_q,
            sample_valid    =>  rx_gen_valid
        );


    rx_mux : process(rx_reset, rx_clock)
    begin
        if( rx_reset = '1' ) then
            mux_streams  <= (others => ZERO_SAMPLE);
            rx_gen_mode  <= '0';
            mux_timestamp      <= (others => '0');
            mux_epoch_complete <= '0';
            mux_ts_from_fir    <= '0';
        elsif( rising_edge(rx_clock) ) then
            -- Registered with the data so the select changes on the same
            -- sample boundary as the mux mode.
            mux_timestamp      <= fir_timestamp;
            mux_epoch_complete <= fir_epoch_complete;
            if( ENABLE_SPATIAL_FIR and rx_mux_mode = RX_MUX_NORMAL ) then
                mux_ts_from_fir <= '1';
            else
                mux_ts_from_fir <= '0';
            end if;

            case rx_mux_mode is
                when RX_MUX_NORMAL =>
                    mux_streams <= fir_streams;
                    if( trigger_signal_out_sync = '0' ) then
                        for i in mux_streams'range loop
                            mux_streams(i).data_v <= '0';
                        end loop;
                    end if;
                when RX_MUX_12BIT_COUNTER | RX_MUX_32BIT_COUNTER =>
                    for i in mux_streams'range loop
                        mux_streams(i).data_i <= rx_gen_i;
                        mux_streams(i).data_q <= rx_gen_q;
                        mux_streams(i).data_v <= rx_gen_valid;
                    end loop;

                    if( rx_mux_mode = RX_MUX_32BIT_COUNTER ) then
                        rx_gen_mode <= '1';
                    else
                        rx_gen_mode <= '0';
                    end if;
                when RX_MUX_ENTROPY =>
                    -- Not yet implemented
                    mux_streams  <= (others => ZERO_SAMPLE);
                when RX_MUX_DIGITAL_LOOPBACK =>
                    mux_streams  <= loopback_streams;
                when others =>
                    mux_streams  <= (others => ZERO_SAMPLE);
            end case;
        end if;
    end process;


    -- RX Trigger
    -- The trigger-control PIO is clocked by sys_clock in NIOS, while the
    -- trigger gates the RX sample path in rx_clock. Deassert arm immediately
    -- on the asynchronous low level and release it only after synchronization
    -- into the sample domain, matching the TX trigger's fail-safe behavior.
    U_reset_sync_rx_trigger_arm : entity work.reset_synchronizer
        generic map (
            INPUT_LEVEL  => '0',
            OUTPUT_LEVEL => '0'
        )
        port map (
            clock => rx_clock,
            async => trigger_arm,
            sync  => trigger_arm_sync
        );

    rxtrig : entity work.trigger(async)
        generic map (
            DEFAULT_OUTPUT  => '0'
        )
        port map (
            armed           => trigger_arm_sync,  -- in  sl
            fired           => trigger_fire,      -- in  sl
            master          => trigger_master,    -- in  sl
            trigger_in      => trigger_line,      -- in  sl
            trigger_out     => trigger_line,      -- out sl
            signal_in       => rx_enable,         -- in  sl
            signal_out      => trigger_signal_out -- out sl
        );


    U_reset_sync_loopback : entity work.reset_synchronizer
        generic map (
            INPUT_LEVEL         => '0',
            OUTPUT_LEVEL        => '0'
        )
        port map (
            clock               =>  loopback_fifo_wclock,
            async               =>  loopback_enabled,
            sync                =>  loopback_fifo_wenabled_i
        );


    U_sync_rxtrig_signal_out : entity work.synchronizer
        generic map (
            RESET_LEVEL =>  '0'
        )
        port map (
            reset       =>  rx_reset,
            clock       =>  rx_clock,
            async       =>  trigger_signal_out,
            sync        =>  trigger_signal_out_sync
        );
        trigger_signal_sync_tb <= trigger_signal_out_sync;

end architecture;
