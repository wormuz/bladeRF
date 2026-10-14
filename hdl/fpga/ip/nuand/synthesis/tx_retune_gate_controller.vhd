library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

-- Bundled NIOS-to-TX-clock mailbox for the timestamped TX retune gate.
-- Command toggles and the 64-bit compare target cross in one handshake image.
entity tx_retune_gate_controller is
    port (
        source_clock       : in  std_logic;
        source_reset       : in  std_logic;
        source_control     : in  std_logic_vector(31 downto 0);
        source_target_lo   : in  std_logic_vector(31 downto 0);
        source_target_hi   : in  std_logic_vector(31 downto 0);

        tx_clock           : in  std_logic;
        tx_reset           : in  std_logic;
        arm                : out std_logic := '0';
        arm_immediate      : out std_logic := '0';
        arm_ack_toggle     : out std_logic := '0';
        target_timestamp   : out unsigned(63 downto 0) := (others => '0');
        complete           : out std_logic := '0';
        abort              : out std_logic := '0';
        fifo_flush_sys     : out std_logic := '0';
        fifo_release_sys   : out std_logic := '0'
    );
end entity;

architecture rtl of tx_retune_gate_controller is
    signal request_tx      : std_logic := '0';
    signal acknowledge_tx  : std_logic;
    signal control_wire_tx : std_logic_vector(95 downto 0);
    signal control_tx     : std_logic_vector(95 downto 0) := (others => '0');
    signal previous_toggles : std_logic_vector(2 downto 0) := (others => '0');
    signal control_initialized : std_logic := '0';
    signal control_init_capture_pending : std_logic := '0';
    signal arm_pending : std_logic := '0';
    signal arm_pending_toggle : std_logic := '0';
    signal arm_compare_lead : std_logic := '0';
    signal previous_flush_toggles_sys : std_logic_vector(1 downto 0) := "00";
    signal flush_control_initialized_sys : std_logic := '0';
begin
    -- Fault flush has to remain commandable when tx_clock (AD9361 data clock)
    -- is absent. Decode only these dedicated NIOS recovery toggles directly
    -- in the source/system domain; normal ARM/COMPLETE/ABORT still use the
    -- bundled TX-clock mailbox below.
    decode_fault_flush_sys : process(source_clock, source_reset)
    begin
        if source_reset = '1' then
            previous_flush_toggles_sys <= "00";
            flush_control_initialized_sys <= '0';
            fifo_flush_sys <= '0';
            fifo_release_sys <= '0';
        elsif rising_edge(source_clock) then
            fifo_flush_sys <= '0';
            fifo_release_sys <= '0';
            if flush_control_initialized_sys = '0' then
                previous_flush_toggles_sys <= source_control(5 downto 4);
                flush_control_initialized_sys <= '1';
            else
                if source_control(4) /= previous_flush_toggles_sys(0) then
                    fifo_flush_sys <= '1';
                end if;
                if source_control(5) /= previous_flush_toggles_sys(1) then
                    fifo_release_sys <= '1';
                end if;
                previous_flush_toggles_sys <= source_control(5 downto 4);
            end if;
        end if;
    end process;

    transfer : entity work.handshake
        generic map (DATA_WIDTH => 96)
        port map (
            source_reset => source_reset,
            source_clock => source_clock,
            source_data  => source_control & source_target_hi &
                            source_target_lo,
            dest_reset   => tx_reset,
            dest_clock   => tx_clock,
            dest_data    => control_wire_tx,
            dest_req     => request_tx,
            dest_ack     => acknowledge_tx
        );

    drive_request : process(tx_clock, tx_reset)
    begin
        if tx_reset = '1' then
            request_tx <= '0';
        elsif rising_edge(tx_clock) then
            if acknowledge_tx = '0' then
                request_tx <= '1';
            else
                request_tx <= '0';
            end if;
        end if;
    end process;

    capture_control : process(tx_clock, tx_reset)
    begin
        if tx_reset = '1' then
            control_tx <= (others => '0');
        elsif rising_edge(tx_clock) then
            if acknowledge_tx = '1' then
                control_tx <= control_wire_tx;
            end if;
        end if;
    end process;

    decode_commands : process(tx_clock, tx_reset)
    begin
        if tx_reset = '1' then
            previous_toggles <= (others => '0');
            control_initialized <= '0';
            control_init_capture_pending <= '0';
            arm_pending <= '0';
            arm_pending_toggle <= '0';
            arm_compare_lead <= '0';
            arm <= '0';
            arm_immediate <= '0';
            arm_ack_toggle <= '0';
            target_timestamp <= (others => '0');
            complete <= '0';
            abort <= '0';
        elsif rising_edge(tx_clock) then
            arm <= '0';
            complete <= '0';
            abort <= '0';
            if control_init_capture_pending = '1' then
                -- Baseline from the destination-clock capture register. The
                -- handshake payload is held until acknowledge returns, but
                -- sampling control_wire_tx directly here creates a second
                -- raw CDC capture path beside capture_control.
                previous_toggles <= control_tx(66 downto 64);
                arm_ack_toggle <= control_tx(64);
                arm_immediate <= control_tx(67);
                target_timestamp <= unsigned(control_tx(63 downto 0));
                control_initialized <= '1';
                control_init_capture_pending <= '0';
                arm_pending <= '0';
                arm_compare_lead <= '0';
            elsif acknowledge_tx = '1' and control_initialized = '0' then
                -- FX3 SYS_RST resets the TX-clock decoder but leaves the
                -- NIOS system and command PIOs running. Adopt the surviving
                -- toggle levels as baseline; do not execute a command from
                -- before reset recovery. capture_control registers the
                -- payload on this edge; baseline it on the following edge.
                control_init_capture_pending <= '1';
            else
                -- control_tx[95:64] is the PIO control word; [63:0] is
                -- the timestamp mailbox. Decode toggles from the control
                -- word, never from low timestamp bits.
                if arm_pending = '1' then
                    if arm_compare_lead = '0' then
                        -- The bank-order comparison has two registered
                        -- stages. Keep ARM pending until both have consumed
                        -- the stable target from this bundled transfer.
                        arm_compare_lead <= '1';
                    else
                        arm <= '1';
                        arm_ack_toggle <= arm_pending_toggle;
                        arm_pending <= '0';
                        arm_compare_lead <= '0';
                    end if;
                end if;
                if control_tx(64) /= previous_toggles(0) then
                    arm_pending <= '1';
                    arm_pending_toggle <= control_tx(64);
                    arm_compare_lead <= '0';
                end if;
                if control_tx(65) /= previous_toggles(1) then
                    complete <= '1';
                end if;
                if control_tx(66) /= previous_toggles(2) then
                    abort <= '1';
                end if;
                previous_toggles <= control_tx(66 downto 64);
                arm_immediate <= control_tx(67);
                target_timestamp <= unsigned(control_tx(63 downto 0));
            end if;
        end if;
    end process;
end architecture;
