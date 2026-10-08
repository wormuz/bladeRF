-- The RX epoch gate deliberately withholds IQ during an RF transition. That
-- interval must not trip fifo_writer's no-progress watchdog, while the same
-- watchdog must resume after the gate admits the next epoch.
library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_writer_epoch_fence_tb is
end entity;

architecture tb of fifo_writer_epoch_fence_tb is
    constant CLK_PERIOD : time := 8 ns;
    signal clock : std_logic := '0';
    signal reset : std_logic := '1';
    signal done : boolean := false;
    signal enable : std_logic := '1';
    signal discard_active : std_logic := '0';
    signal link_start_toggle : std_logic := '0';
    signal sample_ctrls : sample_controls_t(0 to 1) :=
        (others => SAMPLE_CONTROL_DISABLE);
    signal samples : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal fifo_write : std_logic;
    signal fifo_full : std_logic := '0';
    signal fifo_usedw : std_logic_vector(11 downto 0) := (others => '0');
    signal meta_fifo_usedw : std_logic_vector(3 downto 0) := (others => '0');
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
    signal gpif_diag : std_logic_vector(2 downto 0) := (others => '0');
    signal abort_active : std_logic;
    signal epoch_counter : unsigned(7 downto 0);
begin
    clock <= not clock after CLK_PERIOD/2 when not done else '0';

    dut : entity work.fifo_writer
        generic map (
            NUM_STREAMS => 2,
            FIFO_USEDW_WIDTH => 12,
            FIFO_DATA_WIDTH => 64,
            META_FIFO_USEDW_WIDTH => 4,
            META_FIFO_DATA_WIDTH => 128,
            PROGRESS_TIMEOUT_LOG2 => 4
        )
        port map (
            clock => clock,
            reset => reset,
            enable => enable,
            usb_speed => '0',
            meta_en => '1',
            packet_en => '0',
            highly_packed_mode_en => '0',
            timestamp => (others => '0'),
            mini_exp => (others => '0'),
            rx_epoch_discard_active => discard_active,
            in_sample_controls => sample_ctrls,
            in_samples => samples,
            fifo_usedw => fifo_usedw,
            fifo_clear => fifo_clear,
            fifo_write => fifo_write,
            fifo_full => fifo_full,
            fifo_data => fifo_data,
            packet_control => PACKET_CONTROL_DEFAULT,
            packet_ready => packet_ready,
            meta_fifo_full => '0',
            meta_fifo_usedw => meta_fifo_usedw,
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
            gpif_diag => gpif_diag,
            abort_active => abort_active
        );

    stimulus : process
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
        wait until rising_edge(clock);
        wait until link_active = '1';

        discard_active <= '1';
        for i in 1 to 40 loop wait until rising_edge(clock); end loop;
        assert fault_sticky = "000000"
            report "intentional epoch fence tripped the progress watchdog"
            severity failure;

        discard_active <= '0';
        for i in 1 to 24 loop wait until rising_edge(clock); end loop;
        assert fault_sticky(1) = '1'
            report "progress watchdog did not resume after epoch fence"
            severity failure;

        -- Start a clean epoch, prove RX sample writes begin, then hold the
        -- downstream sample FIFO full. The watchdog must distinguish this
        -- backpressure from a link that never produced a sample.
        wait until falling_edge(clock);
        link_start_toggle <= '0';
        for i in 1 to 5 loop wait until rising_edge(clock); end loop;
        for i in 0 to 1 loop
            samples(i).data_i <= to_signed(16#123#, 16);
            samples(i).data_q <= to_signed(-16#123#, 16);
            samples(i).data_v <= '1';
        end loop;
        fifo_usedw <= x"010";
        meta_fifo_usedw <= "1010";
        gpif_diag <= "111";
        wait until fifo_write = '1';
        fifo_full <= '1';
        for i in 1 to 64 loop
            wait until rising_edge(clock);
            exit when fault_sticky(5) = '1';
        end loop;
        assert fault_sticky(2) = '1' and fault_sticky(5) = '1'
            report "full FIFO stall was not classified as GPIF backpressure"
            severity failure;
        assert fault_context(1) = '1' and fault_context(2) = '1'
            report "stall context did not preserve valid input and HOLDOFF state"
            severity failure;
        assert fault_context(13 downto 5) = '0' & fifo_usedw(11 downto 4) and
               fault_context(21 downto 14) = "0000000" & meta_fifo_usedw(3) and
               fault_context(24 downto 22) = "111"
            report "stall context did not preserve quantized sample/META occupancy"
            severity failure;

        -- Reset and begin a fresh epoch, admit one sample, then stop upstream
        -- sample-valid while both FIFOs remain writable. The context snapshot
        -- must show no downstream-full indication and no valid input sample.
        reset <= '1';
        wait for 3*CLK_PERIOD;
        reset <= '0';
        for i in 1 to 10 loop wait until rising_edge(clock); end loop;
        fifo_full <= '0';
        for i in 0 to 1 loop samples(i).data_v <= '1'; end loop;
        wait until falling_edge(clock);
        link_start_toggle <= '1';
        wait until link_active = '1';
        wait until fifo_write = '1';
        for i in 0 to 1 loop samples(i).data_v <= '0'; end loop;
        for i in 1 to 64 loop
            wait until rising_edge(clock);
            exit when fault_sticky(2) = '1';
        end loop;
        assert fault_sticky(2) = '1' and fault_sticky(5) = '0'
            report "non-full stalled sample stream was not captured"
            severity failure;
        assert fault_context(0) = '0' and fault_context(1) = '0' and
               fault_context(3) = '1'
            report "sample-stall context did not distinguish absent valid data"
            severity failure;

        report "fifo_writer_epoch_fence_tb: PASS" severity note;
        done <= true;
        wait;
    end process;
end architecture;
