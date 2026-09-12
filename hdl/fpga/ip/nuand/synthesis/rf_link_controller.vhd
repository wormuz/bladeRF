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

-- RF-link epoch commands used to ride on vendor GPIO bit 6, which is also
-- BLADERF_GPIO_RX_LB_ENABLE in the host header -- a real ABI collision, not
-- a naming accident. This module owns command decode for the new rf_link_cfg
-- PIO (0x9540) so the epoch toggle no longer shares a bit with unrelated
-- vendor GPIO state. It only decodes and pulses/toggles; it does not latch
-- USB speed or size DMA buffers -- that stays in fifo_writer/fifo_reader.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity rf_link_controller is
    port (
        clock              :   in      std_logic;
        reset              :   in      std_logic;

        -- Raw 32-bit word straight from the rf_link_cfg PIO
        cfg_word           :   in      std_logic_vector(31 downto 0);

        -- Legacy vendor GPIO bit 7 (BLADERF_GPIO_FEATURE_SMALL_DMA_XFER),
        -- observed only, never authoritative
        usb_speed_live     :   in      std_logic;

        -- Decoded commands out to the RX/TX domains (cross with synchronizers
        -- OUTSIDE this module, in the top level -- do not instantiate CDC here)
        start_toggle_out   :   out     std_logic := '0';
        stop_toggle_out    :   out     std_logic := '0';
        clear_fault_out    :   out     std_logic := '0';
        requested_speed    :   out     std_logic := '0';

        -- Status back to the host
        start_speed_disagreement :   out     std_logic := '0';  -- sticky
        host_epoch_tag            :   out     std_logic_vector(7 downto 0) := (others => '0')
    );
end entity;

architecture simple of rf_link_controller is

    -- cfg_word bit layout (rf_link_cfg PIO, see nios_system.tcl)
    --   bit 0      requested_usb_speed
    --   bit 1      start toggle
    --   bit 2      stop toggle
    --   bit 3      clear-fault toggle
    --   bits 15:8  host epoch tag

    signal cfg_speed_i        : std_logic := '0';
    signal cfg_start_i        : std_logic := '0';
    signal cfg_stop_i         : std_logic := '0';
    signal cfg_clear_fault_i  : std_logic := '0';
    signal cfg_epoch_tag_i    : std_logic_vector(7 downto 0) := (others => '0');

    signal start_prev         : std_logic := '0';
    signal stop_prev          : std_logic := '0';
    signal clear_fault_prev   : std_logic := '0';

    signal start_toggle_i     : std_logic := '0';
    signal stop_toggle_i      : std_logic := '0';
    signal clear_fault_i      : std_logic := '0';
    signal requested_speed_i  : std_logic := '0';
    signal speed_disagree_i   : std_logic := '0';
    signal epoch_tag_i        : std_logic_vector(7 downto 0) := (others => '0');

begin

    -- Decode the raw PIO word every cycle; this is a plain field split,
    -- not command handling -- edge detection happens below.
    cfg_speed_i       <= cfg_word(0);
    cfg_start_i       <= cfg_word(1);
    cfg_stop_i        <= cfg_word(2);
    cfg_clear_fault_i <= cfg_word(3);
    cfg_epoch_tag_i   <= cfg_word(15 downto 8);

    decode : process( clock, reset )
        variable start_pulse       : std_logic;
        variable stop_pulse        : std_logic;
        variable clear_fault_pulse : std_logic;
    begin
        if( reset = '1' ) then
            start_prev        <= '0';
            stop_prev         <= '0';
            clear_fault_prev  <= '0';
            start_toggle_i    <= '0';
            stop_toggle_i     <= '0';
            clear_fault_i     <= '0';
            requested_speed_i <= '0';
            speed_disagree_i  <= '0';
            epoch_tag_i       <= (others => '0');
        elsif( rising_edge(clock) ) then

            -- One-cycle pulses from a change on each toggle bit. A repeated
            -- write of the same cfg_word (same bit values) produces no
            -- pulse on any of the three commands.
            start_pulse       := '0';
            stop_pulse        := '0';
            clear_fault_pulse := '0';

            if( cfg_start_i /= start_prev ) then
                start_pulse := '1';
            end if;
            if( cfg_stop_i /= stop_prev ) then
                stop_pulse := '1';
            end if;
            if( cfg_clear_fault_i /= clear_fault_prev ) then
                clear_fault_pulse := '1';
            end if;

            start_prev       <= cfg_start_i;
            stop_prev        <= cfg_stop_i;
            clear_fault_prev <= cfg_clear_fault_i;

            requested_speed_i <= cfg_speed_i;
            epoch_tag_i       <= cfg_epoch_tag_i;

            -- stop and clear-fault mirror their toggles unconditionally,
            -- no speed gate applies to them.
            if( stop_pulse = '1' ) then
                stop_toggle_i <= not stop_toggle_i;
            end if;
            if( clear_fault_pulse = '1' ) then
                clear_fault_i <= not clear_fault_i;
            end if;

            -- Fail-closed speed check on start: a start pulse only forwards
            -- if the requested speed matches what the link is actually
            -- running at right now. Disagreement is latched sticky and the
            -- start toggle is withheld -- the host must not be handed a
            -- link epoch whose speed premise is already wrong.
            -- Clear first, then evaluate: a clear-fault pulse forgives the
            -- PREVIOUS disagreement, never a disagreement detected in the
            -- same cycle. Ordered the other way round, a host that cleared
            -- faults and started in one go would see no fault flag and no
            -- link either, with nothing saying why the start was refused.
            if( clear_fault_pulse = '1' ) then
                speed_disagree_i <= '0';
            end if;

            if( start_pulse = '1' ) then
                if( cfg_speed_i /= usb_speed_live ) then
                    speed_disagree_i <= '1';
                else
                    start_toggle_i <= not start_toggle_i;
                end if;
            end if;

        end if;
    end process;

    start_toggle_out          <= start_toggle_i;
    stop_toggle_out           <= stop_toggle_i;
    clear_fault_out           <= clear_fault_i;
    requested_speed           <= requested_speed_i;
    start_speed_disagreement  <= speed_disagree_i;
    host_epoch_tag            <= epoch_tag_i;

end architecture;
