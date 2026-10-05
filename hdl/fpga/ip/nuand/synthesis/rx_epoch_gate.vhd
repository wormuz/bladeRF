-- ADR-0207 BLADE_RF_EVENT_DRIVEN_RF_STATE_001 §6: RX epoch gate.
--
-- Placed between the ADC sample stream and rx_fifo (ADR §6.2 preferred
-- topology: AD9361 RX samples -> timestamp counter attachment ->
-- rx_epoch_gate -> rx_fifo -> packetizer -> FX3/USB). Suppresses USB-
-- packet admission for the duration of a retune transition, then opens
-- immediately once settle_samples ADC samples have been counted past the
-- epoch_complete event -- a real sample-domain counter, not a wall-clock
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
        settle_samples_in    : in  unsigned(31 downto 0) := (others => '0');

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
    constant STATE_SETTLING   : unsigned(3 downto 0) := "0010";
    constant STATE_ACTIVE_NEW : unsigned(3 downto 0) := "0011";
    constant STATE_ERROR      : unsigned(3 downto 0) := "0100";

    signal state            : unsigned(3 downto 0) := STATE_ACTIVE;
    signal active_epoch_id  : unsigned(7 downto 0)  := (others => '0');
    signal settle_remaining : unsigned(31 downto 0) := (others => '0');
    signal settle_target    : unsigned(31 downto 0) := (others => '0');

begin

    gate : process(clock, reset)
        variable any_valid : std_logic;
    begin
        if( reset = '1' ) then
            state               <= STATE_ACTIVE;
            active_epoch_id      <= (others => '0');
            settle_remaining     <= (others => '0');
            settle_target        <= (others => '0');
            out_sample_controls  <= (in_sample_controls'range => SAMPLE_CONTROL_DISABLE);
            out_samples          <= (in_sample_controls'range => ZERO_SAMPLE);
            out_epoch_id         <= (others => '0');
            out_state            <= STATE_ACTIVE;
            out_discard_active   <= '0';
            epoch_start_event    <= '0';
            first_valid_timestamp <= (others => '0');
        elsif( rising_edge(clock) ) then
            epoch_start_event <= '0'; -- single-cycle pulse by default

            -- Any in-sample_controls(i).enable with data_v tells us whether
            -- this cycle actually carries a new ADC sample -- the settle
            -- counter must only advance on real samples, not every clock,
            -- otherwise settle_samples would silently mean something
            -- different at different sample rates.
            any_valid := '0';
            for i in in_sample_controls'range loop
                if( in_sample_controls(i).enable = '1' and in_samples(i).data_v = '1' ) then
                    any_valid := '1';
                end if;
            end loop;

            case state is
                when STATE_ACTIVE =>
                    out_sample_controls <= in_sample_controls;
                    out_samples         <= in_samples;

                    if( epoch_arm = '1' ) then
                        state           <= STATE_PENDING;
                        active_epoch_id <= epoch_id_in;
                    end if;

                when STATE_PENDING =>
                    -- Keep servicing the stream (ADR §6.3 step 1/8: never
                    -- stop consuming USB buffers during retune), but
                    -- suppress admission -- these are pre-retune samples
                    -- still arriving from the old LO.
                    out_sample_controls <= (in_sample_controls'range => SAMPLE_CONTROL_DISABLE);
                    out_samples         <= (in_sample_controls'range => ZERO_SAMPLE);

                    if( epoch_abort = '1' ) then
                        state <= STATE_ACTIVE;
                    elsif( epoch_complete = '1' ) then
                        state            <= STATE_SETTLING;
                        settle_target    <= settle_samples_in;
                        settle_remaining <= settle_samples_in;
                    end if;

                when STATE_SETTLING =>
                    out_sample_controls <= (in_sample_controls'range => SAMPLE_CONTROL_DISABLE);
                    out_samples         <= (in_sample_controls'range => ZERO_SAMPLE);

                    if( epoch_abort = '1' ) then
                        state <= STATE_ACTIVE;
                    elsif( any_valid = '1' ) then
                        if( settle_remaining = 0 ) then
                            state                 <= STATE_ACTIVE_NEW;
                            epoch_start_event      <= '1';
                            first_valid_timestamp  <= rx_timestamp;
                        else
                            settle_remaining <= settle_remaining - 1;
                        end if;
                    end if;

                when STATE_ACTIVE_NEW =>
                    -- One cycle in this state is enough to latch the
                    -- epoch_start_event and first_valid_timestamp; fold
                    -- straight back into ACTIVE so normal admission
                    -- resumes without a second gap.
                    out_sample_controls <= in_sample_controls;
                    out_samples         <= in_samples;
                    state               <= STATE_ACTIVE;

                when others =>
                    -- STATE_ERROR or any unreachable encoding: fail closed,
                    -- never admit samples under an undefined state.
                    out_sample_controls <= (in_sample_controls'range => SAMPLE_CONTROL_DISABLE);
                    out_samples         <= (in_sample_controls'range => ZERO_SAMPLE);
                    if( epoch_abort = '1' ) then
                        state <= STATE_ACTIVE;
                    end if;
            end case;

            out_epoch_id       <= active_epoch_id;
            out_state          <= state;
            if( state = STATE_PENDING or state = STATE_SETTLING ) then
                out_discard_active <= '1';
            else
                out_discard_active <= '0';
            end if;
        end if;
    end process;

end architecture;
