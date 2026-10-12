-- Copyright (c) 2026 Nuand LLC
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

-- Does the TX feed gate hold a burst until its timestamp arrives?
--
-- Two cases, both required:
--   scheduled  a burst timestamped in the future must produce NO valid
--              samples before that timestamp. Packed mode may fill a two-word
--              skid queue before the start edge to align FIFO word boundaries.
--   now        a burst with timestamp 0 (the host's TX_NOW encoding, which
--              the reader turns into all ones by subtracting 1) must start
--              immediately
--
-- The scheduled case regressed when meta_p_time was derived from the
-- registered copy of meta_fifo_data: that copy is all zeros out of reset,
-- and 0 - 1 wraps to all ones, i.e. the META_NOW sentinel, so the gate
-- opened at once for a future burst.
--
-- Run with GHDL:
--   ghdl -a --std=08 fx3_gpif_p.vhd fifo_readwrite_p.vhd \
--                    fifo_reader.vhd fifo_reader_tb.vhd
--   ghdl -e --std=08 fifo_reader_tb
--   ghdl -r --std=08 fifo_reader_tb --stop-time=70us

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;
library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_reader_tb is
    generic (
        -- Sample timestamp for the burst. 0 means "transmit now".
        TGT      : natural := 4000;
        NSTREAMS : natural := 1;
        PACKED   : boolean := false;
        PROGRESS_TIMEOUT_LOG2 : natural := 22
    );
end entity;

architecture sim of fifo_reader_tb is

    function test_fifo_word(word_index : natural)
        return std_logic_vector is
        variable rv : std_logic_vector(63 downto 0);
    begin
        for bit_index in rv'range loop
            if ((word_index * 37 + bit_index * 13 + bit_index / 4) mod 7) < 3 then
                rv(bit_index) := '1';
            else
                rv(bit_index) := '0';
            end if;
        end loop;
        return rv;
    end function;

    -- Interpret each FIFO word as the next 64 bits in the packed SC12Q11
    -- stream. Each sample is I[11:0] followed by Q[11:0].
    function expected_packed_sample(sample_index : natural)
        return std_logic_vector is
        variable rv         : std_logic_vector(23 downto 0);
        variable stream_bit : natural;
        variable word_index : natural;
        variable bit_index  : natural;
    begin
        for sample_bit in rv'range loop
            stream_bit := sample_index * 24 + sample_bit;
            word_index := stream_bit / 64;
            bit_index := stream_bit mod 64;
            rv(sample_bit) := test_fifo_word(word_index)(bit_index);
        end loop;
        return rv;
    end function;

    function sample12(v : std_logic_vector(11 downto 0)) return signed is
    begin
        return resize(signed(v), 16);
    end function;

    signal clock      : std_logic := '0';
    signal reset      : std_logic := '1';
    signal enable     : std_logic := '0';
    signal usb_speed  : std_logic := '0';   -- '0' = SuperSpeed
    signal meta_en    : std_logic := '1';
    signal packet_en  : std_logic := '0';
    signal eight_bit  : std_logic := '0';
    signal packed_en  : std_logic := '0';
    signal timestamp  : unsigned(63 downto 0) := (others => '0');

    signal fifo_usedw : std_logic_vector(11 downto 0) := (others => '1');
    signal fifo_read  : std_logic;
    signal fifo_empty : std_logic := '0';
    signal fifo_data  : std_logic_vector(63 downto 0) := test_fifo_word(0);
    signal fifo_word_index : natural := 0;
    signal fifo_hold  : std_logic := '0';
    signal loopback_data : std_logic_vector(63 downto 0);
    signal loopback_data_valid : std_logic;

    signal pkt_ctrl   : packet_control_t;
    signal pkt_empty  : std_logic;
    signal pkt_ready  : std_logic := '0';

    signal m_usedw    : std_logic_vector(2 downto 0) := "001";
    signal m_read     : std_logic;
    signal m_empty    : std_logic := '0';
    signal m_data     : std_logic_vector(127 downto 0);

    signal in_ctrl    : sample_controls_t(0 to NSTREAMS-1) := (others => SAMPLE_CONTROL_ENABLE);
    signal out_smp    : sample_streams_t(0 to NSTREAMS-1);

    signal uf_led     : std_logic;
    signal uf_count   : unsigned(63 downto 0);
    signal uf_event   : std_logic;
    signal uf_dur     : unsigned(15 downto 0) := x"ffff";
    signal uf_suppress : std_logic := '0';
    signal fault_sticky : std_logic_vector(4 downto 0);

    signal reads_before : natural := 0;
    signal reads_after  : natural := 0;
    signal loopback_reads_before : natural := 0;
    signal loopback_reads_after  : natural := 0;
    signal samples_before : natural := 0;
    signal samples_after  : natural := 0;
    signal first_read_timestamp : unsigned(63 downto 0) := (others => '0');
    signal first_valid_timestamp : unsigned(63 downto 0) := (others => '0');
    signal done         : boolean := false;

    -- Link epoch stimulus: real firmware signals a new epoch via
    -- link_start_toggle before raising enable, else Stage 3's sticky
    -- protocol-error/abort path (enable with no epoch) halts the datapath.
    signal link_start_toggle : std_logic := '0';

    -- Bits 95 downto 32 of the meta header carry the timestamp.
    function hdr(ts : unsigned(63 downto 0)) return std_logic_vector is
        variable v : std_logic_vector(127 downto 0) := (others => '0');
    begin
        v(95 downto 32) := std_logic_vector(ts);
        return v;
    end function;

begin

    clock <= not clock after 5 ns when not done else '0';

    -- The event pulse and episode counter share the same edge detector. This
    -- guards the event interface independently of the much longer LED timer.
    underflow_event_contract : process(clock)
        variable previous_count : unsigned(63 downto 0) := (others => '0');
        variable previous_event : std_logic := '0';
    begin
        if rising_edge(clock) then
            if reset = '1' then
                previous_count := (others => '0');
                previous_event := '0';
            else
                if uf_event = '1' then
                    assert previous_event = '0'
                        report "underflow event was not a one-cycle pulse"
                        severity failure;
                    assert uf_count = previous_count + 1
                        report "underflow event did not match episode counter"
                        severity failure;
                else
                    assert uf_count = previous_count
                        report "underflow counter changed without event"
                        severity failure;
                end if;
                previous_count := uf_count;
                previous_event := uf_event;
            end if;
        end if;
    end process;

    m_data <= hdr(to_unsigned(TGT, 64));

    -- One header only: once it has been read the meta FIFO is empty.
    -- Without this META_LOAD re-arms forever and the later reads would be
    -- legitimate, hiding the defect.
    meta_drain : process(clock)
    begin
        if rising_edge(clock) then
            if reset = '1' then
                m_empty <= '0';
            elsif m_read = '1' then
                m_empty <= '1';
            end if;
        end if;
    end process;

    -- The sample clock runs regardless, as it does in hardware.
    tick : process(clock)
    begin
        if rising_edge(clock) and reset = '0' then
            timestamp <= timestamp + 1;
        end if;
    end process;

    -- Model a show-ahead FIFO: data advances only on an accepted read. This
    -- keeps the head stable while a timestamp gate holds the TX stream.
    fifo_data_model : process(clock)
    begin
        if rising_edge(clock) then
            if reset = '1' then
                fifo_word_index <= 0;
                fifo_data <= test_fifo_word(0);
            elsif fifo_read = '1' and fifo_empty = '0' then
                fifo_word_index <= fifo_word_index + 1;
                fifo_data <= test_fifo_word(fifo_word_index + 1);
            end if;
        end if;
    end process;

    counters : process(clock)
        variable valid_samples : natural;
        variable packed_samples_checked : natural := 0;
        variable loopback_word_checked : natural := 0;
        variable expected_sample : std_logic_vector(23 downto 0);
        variable first_valid_seen : boolean := false;
    begin
        if rising_edge(clock) and reset = '0' and fifo_read = '1' then
            if reads_before = 0 and reads_after = 0 then
                first_read_timestamp <= timestamp;
            end if;
            if timestamp < to_unsigned(TGT, 64) then
                reads_before <= reads_before + 1;
            else
                reads_after <= reads_after + 1;
            end if;
        end if;

        if rising_edge(clock) and reset = '0' and loopback_data_valid = '1' then
            if PACKED then
                assert loopback_data = test_fifo_word(loopback_word_checked)
                    report "packed loopback word order mismatch at word " &
                           integer'image(loopback_word_checked)
                    severity failure;
                loopback_word_checked := loopback_word_checked + 1;
            end if;
            if timestamp < to_unsigned(TGT, 64) then
                loopback_reads_before <= loopback_reads_before + 1;
            else
                loopback_reads_after <= loopback_reads_after + 1;
            end if;
        end if;

        if rising_edge(clock) and reset = '0' and PACKED then
            valid_samples := 0;
            for i in out_smp'range loop
                if out_smp(i).data_v = '1' then
                    valid_samples := valid_samples + 1;
                end if;
            end loop;
            if out_smp(0).data_v = '1' and not first_valid_seen then
                first_valid_timestamp <= timestamp;
                first_valid_seen := true;
            end if;
            if timestamp < to_unsigned(TGT, 64) then
                samples_before <= samples_before + valid_samples;
            else
                samples_after <= samples_after + valid_samples;
            end if;

            if timestamp >= to_unsigned(TGT, 64) and
               out_smp(0).data_v = '1' then
                assert out_smp(1).data_v = '1'
                    report "packed TX streams became valid on different cycles"
                    severity failure;
                if packed_samples_checked < 16 then
                    expected_sample := expected_packed_sample(packed_samples_checked);
                    assert out_smp(0).data_i = sample12(expected_sample(11 downto 0)) and
                           out_smp(0).data_q = sample12(expected_sample(23 downto 12))
                        report "packed TX channel 0 payload mismatch at sample " &
                               integer'image(packed_samples_checked) &
                               " expected=" & to_hstring(expected_sample) &
                               " actual=" & to_hstring(std_logic_vector(out_smp(0).data_q) &
                                                        std_logic_vector(out_smp(0).data_i))
                        severity failure;

                    expected_sample := expected_packed_sample(packed_samples_checked + 1);
                    assert out_smp(1).data_i = sample12(expected_sample(11 downto 0)) and
                           out_smp(1).data_q = sample12(expected_sample(23 downto 12))
                        report "packed TX channel 1 payload mismatch at sample " &
                               integer'image(packed_samples_checked + 1)
                        severity failure;
                    packed_samples_checked := packed_samples_checked + 2;
                end if;
            end if;
        end if;
    end process;

    U_fifo_reader : entity work.fifo_reader
        generic map (
            NUM_STREAMS           => NSTREAMS,
            FIFO_READ_THROTTLE    => 0,
            FIFO_USEDW_WIDTH      => 12,
            FIFO_DATA_WIDTH       => 64,
            META_FIFO_USEDW_WIDTH => 3,
            META_FIFO_DATA_WIDTH  => 128,
            PROGRESS_TIMEOUT_LOG2 => PROGRESS_TIMEOUT_LOG2
        )
        port map (
            clock                 => clock,
            reset                 => reset,
            enable                => enable,
            usb_speed             => usb_speed,
            meta_en               => meta_en,
            packet_en             => packet_en,
            eight_bit_mode_en     => eight_bit,
            highly_packed_mode_en => packed_en,
            timestamp             => timestamp,
            fifo_usedw            => fifo_usedw,
            fifo_read             => fifo_read,
            fifo_empty            => fifo_empty,
            fifo_data             => fifo_data,
            fifo_holdoff          => fifo_hold,
            packet_control        => pkt_ctrl,
            packet_empty          => pkt_empty,
            packet_ready          => pkt_ready,
            meta_fifo_usedw       => m_usedw,
            meta_fifo_read        => m_read,
            meta_fifo_empty       => m_empty,
            meta_fifo_data        => m_data,
            in_sample_controls    => in_ctrl,
            out_samples           => out_smp,
            underflow_led         => uf_led,
            underflow_count       => uf_count,
            underflow_event       => uf_event,
            underflow_duration    => uf_dur,
            underflow_suppress    => uf_suppress,
            link_start_toggle     => link_start_toggle,
            fault_sticky          => fault_sticky,
            loopback_data         => loopback_data,
            loopback_data_valid   => loopback_data_valid
        );

    stim : process
    begin
        wait for 50 ns;
        reset  <= '0';
        -- settle_count: toggle edges are ignored for 8 cycles after reset.
        -- 10*CLK_PERIOD (clock period is 10 ns here) replaces the old 20 ns
        -- settling window; this shifts the read count against the fixed
        -- 60 us window, so the assertions below use reads_before/reads_after
        -- presence checks rather than an exact recorded count -- reported
        -- per instructions rather than adjusting a baseline that doesn't
        -- exist in this bench.
        wait for 10*10 ns;
        enable <= '1';
        if PACKED then
            packed_en <= '1';
            -- The real TX integration asserts this while the loopback FIFO
            -- is full. Packed read-ahead must preserve that backpressure even
            -- though loopback writes now follow logical queue consumption.
            fifo_hold <= '1';
        end if;
        meta_en <= '0';
        fifo_empty <= '1';
        uf_suppress <= '1';
        wait for 100 ns;
        assert uf_count = 0 and uf_led = '0'
            report "intentional TX mute was counted as FIFO underflow"
            severity failure;
        fifo_empty <= '0';
        uf_suppress <= '0';
        meta_en <= '1';
        wait for 10 ns;
        link_start_toggle <= not link_start_toggle;

        if PACKED then
            wait for 100 ns;
            assert reads_before = 0 and fifo_read = '0'
                report "packed read-ahead ignored loopback FIFO holdoff"
                severity failure;
            fifo_hold <= '0';
        end if;

        wait for 60 us;

        report "TGT = " & integer'image(TGT) &
               "  reads before = " & integer'image(reads_before) &
               "  reads after = " & integer'image(reads_after);

        if TGT = 0 then
            assert reads_after > 0
                report "TX_NOW burst never started"
                severity failure;
            report "TX_NOW: feed started";
        else
            if not PACKED then
                assert reads_before = 0
                    report "gate leaked: feed ran before its timestamp"
                    severity failure;
                assert first_read_timestamp = to_unsigned(TGT, 64)
                    report "scheduled TX feed opened at " &
                           to_hstring(std_logic_vector(first_read_timestamp)) &
                           ", expected " & integer'image(TGT)
                    severity failure;
            else
                -- The packed skid queue holds two words while waiting. The
                -- application-visible sample boundary must still be exact.
                assert reads_before = 2
                    report "packed TX did not fill its two-word prefetch queue"
                    severity failure;
                assert loopback_reads_before = 0 and loopback_reads_after > 0
                    report "packed prefetch escaped into loopback before its timestamp"
                    severity failure;
                assert first_valid_timestamp = to_unsigned(TGT, 64)
                    report "scheduled packed samples opened at " &
                           to_hstring(std_logic_vector(first_valid_timestamp)) &
                           ", expected " & integer'image(TGT)
                    severity failure;
            end if;
            assert reads_after > 0
                report "gate never opened at its timestamp"
                severity failure;
            report "scheduled: gate held until the timestamp";
        end if;

        if PACKED then
            assert samples_before = 0
                report "packed TX samples leaked before their scheduled timestamp"
                severity failure;
            assert samples_after > 0
                report "packed TX samples did not resume after their timestamp"
                severity failure;
        end if;
        assert fault_sticky(1) = '0' and fault_sticky(2) = '0'
            report "scheduled TX hold was reported as transport timeout: " &
                   to_hstring(fault_sticky)
            severity failure;

        meta_en <= '0';
        fifo_empty <= '1';
        -- Packed mode can retain two prefetched words after the source FIFO
        -- becomes empty; let the skid queue drain before checking starvation.
        wait for 100 ns;
        assert uf_count = 1 and uf_led = '1'
            report "real TX starvation after resume was not counted"
            severity failure;

        -- Separate two underflow episodes while the visibility LED is still
        -- high. The second event must be observable even though the LED has
        -- no new rising edge; the recovery gate consumes underflow_event.
        uf_suppress <= '1';
        wait for 30 ns;
        uf_suppress <= '0';
        wait for 40 ns;
        assert uf_count = 2 and uf_led = '1'
            report "new underflow episode was hidden by the stretched LED"
            severity failure;

        done <= true;
        wait;
    end process;

end architecture;
