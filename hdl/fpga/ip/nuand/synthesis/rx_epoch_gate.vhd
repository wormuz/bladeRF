-- ADR-0207 BLADE_RF_EVENT_DRIVEN_RF_STATE_001 §6: RX epoch gate.
--
-- Placed between the ADC sample stream and rx_fifo (ADR §6.2 preferred
-- topology: AD9361 RX samples -> timestamp counter attachment ->
-- rx_epoch_gate -> rx_fifo -> packetizer -> FX3/USB). Suppresses IQ
-- admission for the duration of a retune transition, then opens on the first
-- ADC sample after epoch_complete reaches this sample domain. While fenced,
-- zero-IQ keepalive samples preserve USB stream liveness; libbladeRF rejects
-- them because no RX epoch is certified.
-- The epoch boundary is an event, not a sample-count discard or wall-clock
-- delay, and it never resets or re-derives the global RX timestamp
-- (epoch_id is a SEPARATE monotonic counter so the host can tell apart
-- "no new samples yet" from "stream actually broke").
--
-- Copyright (c) 2026 bladeRF project contributors
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

entity rx_epoch_gate is
    generic (
        -- Matches fifo_writer's NUM_STREAMS generic -- both consume the
        -- same adc_controls'length range from rx.vhd, fixed via generic
        -- rather than a port self-reference (VHDL does not allow a port
        -- to reference another port's 'range within the same port
        -- clause; fifo_writer.vhd already works around this the same way).
        NUM_STREAMS : natural := 2
    );
    port (
        clock               : in  std_logic;
        reset               : in  std_logic;

        -- Sample-domain input (post ADC, pre rx_fifo).
        in_sample_controls  : in  sample_controls_t(0 to NUM_STREAMS-1);
        in_samples          : in  sample_streams_t(0 to NUM_STREAMS-1);
        rx_timestamp        : in  unsigned(63 downto 0);

        -- Host control (synchronized by the caller into `clock` domain --
        -- this entity assumes single-cycle pulses already in `clock`,
        -- matching the existing link_start_toggle/link_stop_toggle
        -- convention in rx.vhd rather than inventing a second one).
        epoch_arm           : in  std_logic := '0';
        epoch_complete       : in  std_logic := '0';
        epoch_abort          : in  std_logic := '0';
        epoch_id_in          : in  unsigned(7 downto 0) := (others => '0');

        -- Sample-domain output (into rx_fifo).
        out_sample_controls : out sample_controls_t(0 to NUM_STREAMS-1);
        out_samples         : out sample_streams_t(0 to NUM_STREAMS-1);

        -- Status, mirrors into rx_epoch_status PIO.
        out_epoch_id         : out unsigned(7 downto 0)  := (others => '0');
        out_state            : out unsigned(3 downto 0)  := (others => '0');
        out_discard_active   : out std_logic := '0';
        epoch_start_event    : out std_logic := '0';
        first_valid_timestamp : out unsigned(63 downto 0) := (others => '0')
    );
end entity;

architecture arch of rx_epoch_gate is

    -- Bit-exact with ADR-0207 §6.1 state machine names; numeric encoding
    -- matches RX_EPOCH_STATUS debug states documented in §9.
    constant STATE_ACTIVE     : unsigned(3 downto 0) := "0000";
    constant STATE_PENDING    : unsigned(3 downto 0) := "0001";
    constant STATE_WAIT_FIRST_SAMPLE : unsigned(3 downto 0) := "0010";
    constant STATE_ACTIVE_NEW : unsigned(3 downto 0) := "0011";
    constant STATE_ERROR      : unsigned(3 downto 0) := "0100";

    signal state            : unsigned(3 downto 0) := STATE_ACTIVE;
    signal active_epoch_id  : unsigned(7 downto 0)  := (others => '0');

begin

    gate : process(clock, reset)
        variable any_valid : std_logic;
    begin
        if( reset = '1' ) then
            state               <= STATE_ACTIVE;
            active_epoch_id      <= (others => '0');
            out_sample_controls  <= (in_sample_controls'range => SAMPLE_CONTROL_DISABLE);
            out_samples          <= (in_sample_controls'range => ZERO_SAMPLE);
            out_epoch_id         <= (others => '0');
            out_state            <= STATE_ACTIVE;
            out_discard_active   <= '0';
            epoch_start_event    <= '0';
            first_valid_timestamp <= (others => '0');
        elsif( rising_edge(clock) ) then
            epoch_start_event <= '0'; -- single-cycle pulse by default

            -- Any enabled stream carrying data_v marks a real ADC sample.
            any_valid := '0';
            for i in in_sample_controls'range loop
                if( in_sample_controls(i).enable = '1' and in_samples(i).data_v = '1' ) then
                    any_valid := '1';
                end if;
            end loop;

            case state is
                when STATE_ACTIVE =>
                    if( epoch_abort = '1' ) then
                        out_sample_controls <= (in_sample_controls'range => SAMPLE_CONTROL_DISABLE);
                        out_samples         <= (in_sample_controls'range => ZERO_SAMPLE);
                        state <= STATE_ERROR;
                    else
                        out_sample_controls <= in_sample_controls;
                        out_samples         <= in_samples;
                    end if;

                    if( epoch_arm = '1' and epoch_abort = '0' ) then
                        state           <= STATE_PENDING;
                        active_epoch_id <= epoch_id_in;
                    end if;

                when STATE_PENDING =>
                    -- Keep servicing the stream (ADR §6.3 step 1/8: never
                    -- stop consuming USB buffers during retune), but replace
                    -- old-LO IQ with zero-IQ keepalives so async USB does not
                    -- time out while the host waits for transition events.
                    out_sample_controls <= in_sample_controls;
                    for i in in_samples'range loop
                        out_samples(i) <= (
                            data_i => (others => '0'),
                            data_q => (others => '0'),
                            data_v => in_samples(i).data_v
                        );
                    end loop;

                    if( epoch_abort = '1' ) then
                        -- The RFIC may already have changed even when the
                        -- host reports failure. Never reopen samples under
                        -- the aborted epoch (or pretend the old LO returned).
                        state <= STATE_ERROR;
                    elsif( epoch_complete = '1' ) then
                        -- Completion only arms the new epoch.  The RX
                        -- sample-valid pulse is asynchronous to this
                        -- control event, so wait for an actual sample
                        -- before publishing ACTIVE_NEW to the host.
                        state <= STATE_WAIT_FIRST_SAMPLE;
                    end if;

                when STATE_WAIT_FIRST_SAMPLE =>
                    -- This state is event-waiting, not a fixed settling
                    -- delay. Admit and timestamp the first real sample on
                    -- the same edge, then publish ACTIVE_NEW for one cycle
                    -- so the host cannot observe success before the
                    -- timestamp latch is valid.
                    out_sample_controls <= in_sample_controls;
                    for i in in_samples'range loop
                        out_samples(i) <= (
                            data_i => (others => '0'),
                            data_q => (others => '0'),
                            data_v => in_samples(i).data_v
                        );
                    end loop;

                    if( epoch_abort = '1' ) then
                        state <= STATE_ERROR;
                    elsif( any_valid = '1' ) then
                        out_sample_controls  <= in_sample_controls;
                        out_samples          <= in_samples;
                        epoch_start_event    <= '1';
                        first_valid_timestamp <= rx_timestamp;
                        state <= STATE_ACTIVE_NEW;
                    end if;

                when STATE_ACTIVE_NEW =>
                    -- The first sample was admitted and timestamped in
                    -- STATE_WAIT_FIRST_SAMPLE on the preceding edge. Hold the new
                    -- epoch active and expose the completion marker.
                    if( epoch_abort = '1' ) then
                        out_sample_controls <= (in_sample_controls'range => SAMPLE_CONTROL_DISABLE);
                        out_samples         <= (in_sample_controls'range => ZERO_SAMPLE);
                        state <= STATE_ERROR;
                    else
                        out_sample_controls <= in_sample_controls;
                        out_samples         <= in_samples;
                        state               <= STATE_ACTIVE;
                    end if;

                when others =>
                    -- STATE_ERROR is fail-closed for IQ: only zero-IQ
                    -- keepalives continue to the USB transport so an
                    -- invalidated continuous stream remains restartable.
                    -- The host epoch certificate is revoked until a later
                    -- explicit ARM/COMPLETE transaction admits real samples.
                    out_sample_controls <= in_sample_controls;
                    for i in in_samples'range loop
                        out_samples(i) <= (
                            data_i => (others => '0'),
                            data_q => (others => '0'),
                            data_v => in_samples(i).data_v
                        );
                    end loop;
                    if( epoch_abort = '1' ) then
                        state <= STATE_ERROR;
                    elsif( epoch_arm = '1' ) then
                        active_epoch_id <= epoch_id_in;
                        state <= STATE_PENDING;
                    end if;
            end case;

            out_epoch_id       <= active_epoch_id;
            out_state          <= state;
            if( state = STATE_PENDING or state = STATE_WAIT_FIRST_SAMPLE or
                state = STATE_ERROR ) then
                out_discard_active <= '1';
            else
                out_discard_active <= '0';
            end if;
        end if;
    end process;

end architecture;
