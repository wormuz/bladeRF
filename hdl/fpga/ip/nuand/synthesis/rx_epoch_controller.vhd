library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

-- Cross and decode the bundled NIOS RX epoch command word. The same captured
-- word carries command toggles and epoch identity so a command cannot be
-- applied with an epoch ID from another transfer.
entity rx_epoch_controller is
    port (
        source_clock       : in  std_logic;
        source_reset       : in  std_logic;
        source_control     : in  std_logic_vector(31 downto 0);

        rx_clock           : in  std_logic;
        rx_reset           : in  std_logic;
        rx_control         : out std_logic_vector(31 downto 0);
        epoch_arm          : out std_logic;
        epoch_complete     : out std_logic;
        epoch_abort        : out std_logic;
        epoch_id           : out unsigned(7 downto 0);
        meta_enable        : out std_logic;
        complete_seen      : out std_logic
    );
end entity;

architecture rtl of rx_epoch_controller is
    signal request_rx       : std_logic := '0';
    signal acknowledge_rx   : std_logic;
    signal control_wire_rx  : std_logic_vector(31 downto 0);
    signal control_rx       : std_logic_vector(31 downto 0) := (others => '0');
    signal previous_toggles : std_logic_vector(2 downto 0) := (others => '0');
    signal arm_rx           : std_logic := '0';
    signal complete_rx      : std_logic := '0';
    signal abort_rx         : std_logic := '0';
    signal epoch_id_rx      : unsigned(7 downto 0) := (others => '0');
    signal meta_enable_rx   : std_logic := '0';
    signal complete_seen_rx : std_logic := '0';
begin
    transfer : entity work.handshake
        generic map (DATA_WIDTH => 32)
        port map (
            source_reset => source_reset,
            source_clock => source_clock,
            source_data  => source_control,
            dest_reset   => rx_reset,
            dest_clock   => rx_clock,
            dest_data    => control_wire_rx,
            dest_req     => request_rx,
            dest_ack     => acknowledge_rx
        );

    -- The handshake request cycles continuously. This keeps the most recent
    -- control word available without adding a second command mailbox.
    drive_request : process(rx_clock, rx_reset)
    begin
        if rx_reset = '1' then
            request_rx <= '0';
        elsif rising_edge(rx_clock) then
            if acknowledge_rx = '0' then
                request_rx <= '1';
            else
                request_rx <= '0';
            end if;
        end if;
    end process;

    capture_control : process(rx_clock, rx_reset)
    begin
        if rx_reset = '1' then
            control_rx <= (others => '0');
        elsif rising_edge(rx_clock) then
            if acknowledge_rx = '1' then
                control_rx <= control_wire_rx;
            end if;
        end if;
    end process;

    decode_toggles : process(rx_clock, rx_reset)
    begin
        if rx_reset = '1' then
            previous_toggles <= (others => '0');
            arm_rx <= '0';
            complete_rx <= '0';
            abort_rx <= '0';
            epoch_id_rx <= (others => '0');
            meta_enable_rx <= '0';
            complete_seen_rx <= '0';
        elsif rising_edge(rx_clock) then
            arm_rx <= '0';
            complete_rx <= '0';
            abort_rx <= '0';

            if control_rx(0) /= previous_toggles(0) then
                arm_rx <= '1';
            end if;
            if control_rx(1) /= previous_toggles(1) then
                complete_rx <= '1';
            end if;
            if control_rx(2) /= previous_toggles(2) then
                abort_rx <= '1';
            end if;

            previous_toggles <= control_rx(2 downto 0);
            epoch_id_rx <= unsigned(control_rx(15 downto 8));
            meta_enable_rx <= control_rx(16);

            if arm_rx = '1' then
                complete_seen_rx <= '0';
            elsif complete_rx = '1' then
                complete_seen_rx <= '1';
            end if;
        end if;
    end process;

    rx_control <= control_rx;
    epoch_arm <= arm_rx;
    epoch_complete <= complete_rx;
    epoch_abort <= abort_rx;
    epoch_id <= epoch_id_rx;
    meta_enable <= meta_enable_rx;
    complete_seen <= complete_seen_rx;
end architecture;
