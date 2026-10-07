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
    signal fifo_clear : std_logic;
    signal fifo_data : std_logic_vector(63 downto 0);
    signal meta_write : std_logic;
    signal meta_data : std_logic_vector(127 downto 0);
    signal packet_ready : std_logic;
    signal overflow_led : std_logic;
    signal overflow_count : unsigned(63 downto 0);
    signal link_active : std_logic;
    signal fault_sticky : std_logic_vector(4 downto 0);
    signal abort_active : std_logic;
    signal epoch_counter : unsigned(7 downto 0);
begin
    clock <= not clock after CLK_PERIOD/2 when not done else '0';

    dut : entity work.fifo_writer
        generic map (
            NUM_STREAMS => 2,
            FIFO_USEDW_WIDTH => 4,
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
        assert fault_sticky = "00000"
            report "intentional epoch fence tripped the progress watchdog"
            severity failure;

        discard_active <= '0';
        for i in 1 to 24 loop wait until rising_edge(clock); end loop;
        assert fault_sticky(1) = '1'
            report "progress watchdog did not resume after epoch fence"
            severity failure;

        report "fifo_writer_epoch_fence_tb: PASS" severity note;
        done <= true;
        wait;
    end process;
end architecture;
