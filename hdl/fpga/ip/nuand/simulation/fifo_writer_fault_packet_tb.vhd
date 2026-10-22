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

-- META/sample pairing across an RX epoch gate fault (RX_X2).
--
-- fx3_gpif reads the two FIFOs positionally: one META header, then a fixed
-- number of sample words. A packet whose header was committed must therefore
-- receive exactly that many sample words, fault or not. A packet cut short
-- leaves its words in the sample FIFO, and every later header is read with
-- samples (K - m) ticks newer than its timestamp, accumulating per fault
-- until the RX FIFOs are reset (stream restart).
--
-- Each sample carries its own timestamp in data_i, so the first word after
-- every header must name that header's timestamp, and every complete packet
-- holds the word count of a fault-free packet. The fault is swept across
-- every cycle of a packet, including the header-commit cycle.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_writer_fault_packet_tb is
end entity;

architecture tb of fifo_writer_fault_packet_tb is

    constant CLK_PERIOD : time := 8 ns;

    signal clock : std_logic := '0';
    signal reset : std_logic := '1';
    signal done : boolean := false;
    signal epoch_gate_fault : std_logic := '0';
    signal epoch_id : unsigned(7 downto 0) := x"01";
    signal link_start_toggle : std_logic := '0';
    signal sample_ctrls : sample_controls_t(0 to 1) :=
        (others => SAMPLE_CONTROL_DISABLE);
    signal samples : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal timestamp : unsigned(63 downto 0) := (others => '0');
    signal fifo_write : std_logic;
    signal fifo_clear : std_logic;
    signal fifo_data : std_logic_vector(63 downto 0);
    signal meta_write : std_logic;
    signal meta_data : std_logic_vector(127 downto 0);
    signal packet_ready : std_logic;
    signal overflow_led : std_logic;
    signal overflow_count : unsigned(63 downto 0);
    signal link_active : std_logic;
    signal fault_sticky : std_logic_vector(5 downto 0);
    signal fault_context : std_logic_vector(24 downto 0);
    signal abort_active : std_logic;
    signal epoch_counter : unsigned(7 downto 0);

    -- Packet bookkeeping in commit order (what fx3_gpif will read).
    signal words_in_packet : natural := 0;
    signal packets : natural := 0;
    signal packet_words : natural := 0;     -- fault-free packet length
    signal header_ts : unsigned(15 downto 0) := (others => '0');
    signal want_first : boolean := false;
    signal errors : natural := 0;
begin
    clock <= not clock after CLK_PERIOD/2 when not done else '0';

    dut : entity work.fifo_writer
        generic map (
            NUM_STREAMS => 2,
            FIFO_USEDW_WIDTH => 12,
            FIFO_DATA_WIDTH => 64,
            META_FIFO_USEDW_WIDTH => 4,
            META_FIFO_DATA_WIDTH => 128,
            PROGRESS_TIMEOUT_LOG2 => 20
        )
        port map (
            clock => clock,
            reset => reset,
            enable => '1',
            usb_speed => '1',
            meta_en => '1',
            packet_en => '0',
            highly_packed_mode_en => '0',
            timestamp => timestamp,
            mini_exp => (others => '0'),
            rx_epoch_meta_enable => '1',
            rx_epoch_id => epoch_id,
            rx_epoch_discard_active => epoch_gate_fault,
            rx_epoch_gate_fault => epoch_gate_fault,
            in_sample_controls => sample_ctrls,
            in_samples => samples,
            fifo_usedw => (others => '0'),
            fifo_clear => fifo_clear,
            fifo_write => fifo_write,
            fifo_full => '0',
            fifo_data => fifo_data,
            packet_control => PACKET_CONTROL_DEFAULT,
            packet_ready => packet_ready,
            meta_fifo_full => '0',
            meta_fifo_usedw => (others => '0'),
            meta_fifo_data => meta_data,
            meta_fifo_write => meta_write,
            overflow_led => overflow_led,
            overflow_count => overflow_count,
            overflow_duration => (others => '0'),
            link_start_toggle => link_start_toggle,
            link_active => link_active,
            link_epoch_counter => epoch_counter,
            fault_sticky => fault_sticky,
            fault_context => fault_context,
            gpif_diag => (others => '0'),
            abort_active => abort_active
        );

    -- Sample source like rx_clock: one valid tuple every other cycle, each
    -- carrying the timestamp the gate attaches to it.
    source : process(clock)
        variable tick : std_logic := '0';
    begin
        if rising_edge(clock) then
            tick := not tick;
            for i in 0 to 1 loop
                samples(i).data_v <= tick;
                samples(i).data_i <= signed(timestamp(15 downto 0) + 1);
                samples(i).data_q <= to_signed(i, 16);
            end loop;
            if tick = '1' then
                timestamp <= timestamp + 1;
            end if;
        end if;
    end process;

    checker : process(clock)
        variable ts : unsigned(15 downto 0);
    begin
        if rising_edge(clock) and reset = '0' then
            if meta_write = '1' then
                if packets >= 2 then
                    if packets = 2 then
                        packet_words <= words_in_packet;
                    elsif words_in_packet /= packet_words then
                        report "packet " & integer'image(packets) & " holds " &
                               integer'image(words_in_packet) & " sample words, fault-free packet holds " &
                               integer'image(packet_words)
                            severity error;
                        errors <= errors + 1;
                    end if;
                end if;
                packets <= packets + 1;
                words_in_packet <= 0;
                header_ts <= unsigned(meta_data(47 downto 32));
                want_first <= true;
            end if;
            if fifo_write = '1' then
                ts := unsigned(fifo_data(15 downto 0));
                if meta_write = '1' then
                    report "sample word committed on the header cycle" severity error;
                    errors <= errors + 1;
                elsif want_first and unsigned(fifo_data) /= 0 then
                    if ts /= header_ts then
                        report "first sample of packet " & integer'image(packets) &
                               " has timestamp " & integer'image(to_integer(ts)) & ", header names " &
                               integer'image(to_integer(header_ts))
                            severity error;
                        errors <= errors + 1;
                    end if;
                    want_first <= false;
                end if;
                if meta_write = '0' then
                    words_in_packet <= words_in_packet + 1;
                end if;
            end if;
        end if;
    end process;

    stimulus : process
        variable start_packets : natural;
        variable offset : natural;
    begin
        wait for 10*CLK_PERIOD;
        wait until rising_edge(clock);
        reset <= '0';
        for i in 1 to 10 loop wait until rising_edge(clock); end loop;
        for i in 0 to 1 loop
            sample_ctrls(i).enable <= '1';
            sample_ctrls(i).data_req <= '1';
        end loop;
        wait until rising_edge(clock);
        link_start_toggle <= '1';
        wait until link_active = '1';

        -- The first packet is sized before the latched USB speed reaches
        -- dma_buf_size; the second fixes the reference length.
        wait until packets = 4;
        assert packet_words > 0 report "no reference packet" severity failure;

        -- Fault around the header commit, inside the packet and around its
        -- last word (a packet spans 2*packet_words cycles at this rate).
        for k in 0 to 15 loop
            if k <= 7 then
                offset := k;
            elsif k <= 9 then
                offset := 257 + 300 * (k - 8);
            else
                offset := 2*packet_words - 13 + k;
            end if;
            start_packets := packets;
            wait until rising_edge(clock) and meta_write = '1';
            for i in 1 to offset loop wait until rising_edge(clock); end loop;
            wait until falling_edge(clock);
            epoch_gate_fault <= '1';
            for i in 1 to 37 loop wait until rising_edge(clock); end loop;
            wait until falling_edge(clock);
            epoch_gate_fault <= '0';
            epoch_id <= epoch_id + 1;
            wait until packets >= start_packets + 3;
        end loop;

        assert errors = 0
            report "META/sample pairing broken by an RX epoch gate fault: " & integer'image(errors) & " errors"
            severity failure;
        report "fifo_writer_fault_packet_tb: " & integer'image(packets) & " packets, " &
               integer'image(packet_words) & " words each, pairing intact" severity note;
        done <= true;
        wait;
    end process;

end architecture;
