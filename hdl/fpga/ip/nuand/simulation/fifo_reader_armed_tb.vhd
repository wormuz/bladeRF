-- fifo_reader: enable-before-START is ARMED, mirroring fifo_writer_armed_tb.
--
-- FX3 raises TX enable in the same vendor command that resets the fabric,
-- so the epoch always arrives after enable. The reader must hold -- no
-- reads, no fault -- until the START, and flag only a START on a dead
-- datapath.
--
--   1. enable high, no epoch: no violation, no sticky fault, no abort,
--      no FIFO reads.
--   2. START: link up, reads flow.
--   3. enable low, then START: protocol violation, fault_sticky(3).

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.fx3_gpif_p.all;

entity fifo_reader_armed_tb is
end entity;

architecture tb of fifo_reader_armed_tb is

    constant NSTREAMS   : natural := 1;
    constant CLK_PERIOD : time    := 8 ns;

    signal clock      : std_logic := '0';
    signal reset      : std_logic := '1';
    signal enable     : std_logic := '0';
    signal timestamp  : unsigned(63 downto 0) := (others => '0');

    -- FIFO always reports data available: if the reader wants to read, it
    -- can, so any read in ARMED is the reader's decision, not starvation.
    signal fifo_usedw : std_logic_vector(11 downto 0) := (others => '1');
    signal fifo_read  : std_logic;
    signal fifo_data  : std_logic_vector(63 downto 0) := x"0123456789ABCDEF";

    signal pkt_ctrl   : packet_control_t;
    signal pkt_empty  : std_logic;

    signal m_usedw    : std_logic_vector(2 downto 0) := "001";
    signal m_read     : std_logic;
    signal m_data     : std_logic_vector(127 downto 0) := (others => '0');

    signal in_ctrl    : sample_controls_t(0 to NSTREAMS-1) := (others => SAMPLE_CONTROL_ENABLE);
    signal out_smp    : sample_streams_t(0 to NSTREAMS-1);

    signal uf_led     : std_logic;
    signal uf_count   : unsigned(63 downto 0);

    signal link_start_toggle  : std_logic := '0';
    signal link_active        : std_logic;
    signal protocol_start_violation : std_logic;
    signal fault_sticky       : std_logic_vector(4 downto 0);
    signal abort_active       : std_logic;

    signal reads_seen : natural := 0;
    signal done       : boolean := false;

begin

    clock <= not clock after CLK_PERIOD/2 when not done else '0';

    U_dut : entity work.fifo_reader
        generic map (
            NUM_STREAMS           => NSTREAMS,
            FIFO_READ_THROTTLE    => 0,
            FIFO_USEDW_WIDTH      => 12,
            FIFO_DATA_WIDTH       => 64,
            META_FIFO_USEDW_WIDTH => 3,
            META_FIFO_DATA_WIDTH  => 128
        )
        port map (
            clock                 => clock,
            reset                 => reset,
            enable                => enable,
            usb_speed             => '0',
            meta_en               => '0',
            packet_en             => '0',
            eight_bit_mode_en     => '0',
            highly_packed_mode_en => '0',
            timestamp             => timestamp,
            fifo_usedw            => fifo_usedw,
            fifo_read             => fifo_read,
            fifo_empty            => '0',
            fifo_data             => fifo_data,
            fifo_holdoff          => '0',
            packet_control        => pkt_ctrl,
            packet_empty          => pkt_empty,
            packet_ready          => '0',
            meta_fifo_usedw       => m_usedw,
            meta_fifo_read        => m_read,
            meta_fifo_empty       => '0',
            meta_fifo_data        => m_data,
            in_sample_controls    => in_ctrl,
            out_samples           => out_smp,
            underflow_led         => uf_led,
            underflow_count       => uf_count,
            underflow_duration    => x"ffff",
            link_start_toggle     => link_start_toggle,
            link_active           => link_active,
            protocol_start_violation => protocol_start_violation,
            fault_sticky          => fault_sticky,
            abort_active          => abort_active
        );

    ts_proc : process(clock)
    begin
        if rising_edge(clock) and reset = '0' then
            timestamp <= timestamp + 1;
        end if;
    end process;

    count_proc : process(clock)
    begin
        if rising_edge(clock) and fifo_read = '1' then
            reads_seen <= reads_seen + 1;
        end if;
    end process;

    stim : process
        variable reads_at_start : natural;
    begin
        reset <= '1';
        wait for 10*CLK_PERIOD;
        wait until rising_edge(clock);
        reset <= '0';
        wait until rising_edge(clock);

        ------------------------------------------------------------------
        -- 1. enable first, no epoch: ARMED.
        ------------------------------------------------------------------
        enable <= '1';
        for i in 1 to 200 loop
            wait until rising_edge(clock);
        end loop;

        assert protocol_start_violation = '0'
            report "case 1: enable before START flagged as a violation"
            severity error;
        assert fault_sticky = "00000"
            report "case 1: sticky fault set while merely armed"
            severity error;
        assert abort_active = '0'
            report "case 1: abort asserted while merely armed"
            severity error;
        assert link_active = '0'
            report "case 1: link reported active without any START"
            severity error;
        assert reads_seen = 0
            report "case 1: " & integer'image(reads_seen)
                 & " FIFO reads before any epoch"
            severity error;
        report "case 1 OK: enable without epoch is ARMED -- no fault, no reads";

        ------------------------------------------------------------------
        -- 2. START while enabled: stream.
        ------------------------------------------------------------------
        link_start_toggle <= not link_start_toggle;
        for i in 1 to 200 loop
            wait until rising_edge(clock);
        end loop;

        assert link_active = '1'
            report "case 2: link did not come up on START"
            severity error;
        assert reads_seen > 0
            report "case 2: no FIFO reads after START"
            severity error;
        assert fault_sticky = "00000"
            report "case 2: sticky fault after a clean START"
            severity error;
        report "case 2 OK: START releases ARMED, " & integer'image(reads_seen)
             & " reads";

        ------------------------------------------------------------------
        -- 3. START on a dead datapath: the real violation.
        ------------------------------------------------------------------
        enable <= '0';
        for i in 1 to 20 loop
            wait until rising_edge(clock);
        end loop;
        reads_at_start := reads_seen;

        link_start_toggle <= not link_start_toggle;
        for i in 1 to 50 loop
            wait until rising_edge(clock);
        end loop;

        assert protocol_start_violation = '1'
            report "case 3: START with enable low not flagged as a violation"
            severity error;
        assert fault_sticky(3) = '1'
            report "case 3: fault_sticky(3) not set for a START on a dead datapath"
            severity error;
        assert reads_seen = reads_at_start
            report "case 3: FIFO read with enable low"
            severity error;
        report "case 3 OK: START while disabled is the protocol violation";

        report "fifo_reader_armed_tb: all cases passed";
        done <= true;
        wait;
    end process;

end architecture;
