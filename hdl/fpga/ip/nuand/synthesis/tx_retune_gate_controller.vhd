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
        abort              : out std_logic := '0'
    );
end entity;

architecture rtl of tx_retune_gate_controller is
    signal request_tx      : std_logic := '0';
    signal acknowledge_tx  : std_logic;
    signal control_wire_tx : std_logic_vector(95 downto 0);
    signal control_tx     : std_logic_vector(95 downto 0) := (others => '0');
    signal previous_toggles : std_logic_vector(2 downto 0) := (others => '0');
    signal control_initialized : std_logic := '0';
begin
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
            if acknowledge_tx = '1' and control_initialized = '0' then
                -- FX3 SYS_RST resets the TX-clock decoder but leaves the
                -- NIOS system and command PIOs running. Adopt the surviving
                -- toggle levels as baseline; do not execute a command from
                -- before reset recovery.
                previous_toggles <= control_wire_tx(66 downto 64);
                arm_ack_toggle <= control_wire_tx(64);
                arm_immediate <= control_wire_tx(67);
                target_timestamp <= unsigned(control_wire_tx(63 downto 0));
                control_initialized <= '1';
            else
                -- control_tx[95:64] is the PIO control word; [63:0] is
                -- the timestamp mailbox. Decode toggles from the control
                -- word, never from low timestamp bits.
                if control_tx(64) /= previous_toggles(0) then
                    arm <= '1';
                    arm_ack_toggle <= control_tx(64);
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
