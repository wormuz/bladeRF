-- Fault-only barrier for TX sample and metadata DCFIFOs.
-- The controller and its command/status path live in sys_clock so a missing
-- AD9361 data clock cannot prevent entry into the fail-closed drain state.
-- Normal timestamped retunes must never use this block.

library ieee;
    use ieee.std_logic_1164.all;

entity tx_fifo_fault_flush is
    port (
        sys_clock         : in  std_logic;
        sys_reset         : in  std_logic;
        tx_clock          : in  std_logic;
        tx_reset          : in  std_logic;
        write_clock       : in  std_logic;
        start             : in  std_logic;
        release_request   : in  std_logic;
        gpif_idle         : in  std_logic;
        sample_wempty     : in  std_logic;
        meta_wempty       : in  std_logic;
        sample_rempty     : in  std_logic;
        meta_rempty       : in  std_logic;
        fifo_aclr         : out std_logic;
        write_block       : out std_logic;
        reader_block      : out std_logic;
        busy              : out std_logic;
        done              : out std_logic;
        error             : out std_logic;
        empty_confirmed   : out std_logic;
        release_complete  : out std_logic;
        write_drain_ready : out std_logic
    );
end entity;

architecture rtl of tx_fifo_fault_flush is
    type state_t is (IDLE, ASSERT_CLEAR, HELD, WAIT_TX_CLOCK_BASELINE,
                     WAIT_TX_CLOCK, RELEASE_WAIT);
    signal state                : state_t := IDLE;
    signal reset_hold           : std_logic := '0';
    signal fifo_aclr_i          : std_logic;
    signal busy_i               : std_logic := '0';
    signal done_i               : std_logic := '0';
    signal error_i              : std_logic := '0';
    signal released_i           : std_logic := '0';
    signal write_block_i        : std_logic;
    signal write_block_seen     : std_logic := '0';
    signal write_block_seen_reset : std_logic;
    signal write_block_seen_sys: std_logic;
    signal write_release_pipe   : std_logic_vector(2 downto 0) := (others => '0');
    signal read_release_pipe    : std_logic_vector(2 downto 0) := (others => '0');
    signal write_release_reset  : std_logic;
    signal read_release_reset   : std_logic;
    signal write_empty_ready    : std_logic;
    signal write_empty_ready_sys: std_logic;
    signal read_empty_ready     : std_logic;
    signal read_empty_ready_sys : std_logic;
    signal tx_reset_sys         : std_logic;
    signal tx_clock_alive_toggle: std_logic := '0';
    signal tx_clock_alive_sys   : std_logic;
    signal tx_clock_alive_seen  : std_logic := '0';
    signal tx_clock_edges_after_release : natural range 0 to 3 := 0;
    signal tx_clock_baseline_wait : natural range 0 to 2 := 0;
begin
    -- DCFIFO asynchronous clear assertion is safe without either local clock.
    -- Keep it asserted until the host has restored the RFIC and explicitly
    -- requests release. The read-side ready pipeline is asynchronously reset
    -- by this same signal, so a stopped tx_clock can never leave a stale
    -- "empty and ready" indication accepted as post-clear evidence.
    fifo_aclr_i <= tx_reset or reset_hold;
    fifo_aclr <= fifo_aclr_i;
    write_block <= write_block_i;
    reader_block <= busy_i;
    busy <= busy_i;
    done <= done_i;
    error <= error_i;
    empty_confirmed <= released_i and not tx_reset_sys;
    release_complete <= released_i and not tx_reset_sys;
    write_drain_ready <= busy_i and write_block_seen_sys and
                         reset_hold and not tx_reset_sys and gpif_idle;

    U_write_block_sync : entity work.reset_synchronizer
        generic map (INPUT_LEVEL => '1', OUTPUT_LEVEL => '1')
        port map (
            clock => write_clock,
            async => sys_reset or tx_reset or busy_i,
            sync => write_block_i
        );

    U_write_block_seen_reset_sync : entity work.reset_synchronizer
        generic map (INPUT_LEVEL => '1', OUTPUT_LEVEL => '1')
        port map (
            clock => write_clock,
            async => sys_reset,
            sync => write_block_seen_reset
        );

    process(write_clock, write_block_seen_reset)
    begin
        if write_block_seen_reset = '1' then
            write_block_seen <= '0';
        elsif rising_edge(write_clock) then
            write_block_seen <= write_block_i;
        end if;
    end process;

    U_write_block_seen_sync : entity work.synchronizer
        generic map (RESET_LEVEL => '0')
        port map (
            reset => sys_reset,
            clock => sys_clock,
            async => write_block_seen,
            sync => write_block_seen_sys
        );

    -- The FIFO clear is released in sys_clock. Keep the local evidence
    -- pipeline in reset until its own clock domain has safely observed that
    -- release; otherwise the async reset deassertion itself has recovery
    -- violations at these status flops.
    U_write_release_reset_sync : entity work.reset_synchronizer
        generic map (INPUT_LEVEL => '1', OUTPUT_LEVEL => '1')
        port map (
            clock => write_clock,
            async => fifo_aclr_i,
            sync => write_release_reset
        );

    process(write_clock, write_release_reset)
    begin
        if write_release_reset = '1' then
            write_release_pipe <= (others => '0');
        elsif rising_edge(write_clock) then
            write_release_pipe <= write_release_pipe(1 downto 0) & '1';
        end if;
    end process;

    write_empty_ready <= '1' when write_release_pipe = "111" and
                                  sample_wempty = '1' and
                                  meta_wempty = '1' else '0';

    U_write_empty_ready_sync : entity work.synchronizer
        generic map (RESET_LEVEL => '0')
        port map (
            reset => sys_reset,
            clock => sys_clock,
            async => write_empty_ready,
            sync => write_empty_ready_sys
        );

    U_read_release_reset_sync : entity work.reset_synchronizer
        generic map (INPUT_LEVEL => '1', OUTPUT_LEVEL => '1')
        port map (
            clock => tx_clock,
            async => fifo_aclr_i,
            sync => read_release_reset
        );

    process(tx_clock, read_release_reset)
    begin
        if read_release_reset = '1' then
            read_release_pipe <= (others => '0');
        elsif rising_edge(tx_clock) then
            read_release_pipe <= read_release_pipe(1 downto 0) & '1';
        end if;
    end process;

    read_empty_ready <= '1' when read_release_pipe = "111" and
                                 sample_rempty = '1' and
                                 meta_rempty = '1' else '0';

    U_read_empty_ready_sync : entity work.synchronizer
        generic map (RESET_LEVEL => '0')
        port map (
            reset => sys_reset,
            clock => sys_clock,
            async => read_empty_ready,
            sync => read_empty_ready_sys
        );

    -- A release request is accepted only after new TX-clock edges have been
    -- observed *after* that request. Historical/stale clock-alive state cannot
    -- release the asynchronous clear after a later RFIC clock loss.
    process(tx_clock, tx_reset)
    begin
        if tx_reset = '1' then
            tx_clock_alive_toggle <= '0';
        elsif rising_edge(tx_clock) then
            tx_clock_alive_toggle <= not tx_clock_alive_toggle;
        end if;
    end process;

    U_tx_clock_alive_sync : entity work.synchronizer
        generic map (RESET_LEVEL => '0')
        port map (
            reset => sys_reset,
            clock => sys_clock,
            async => tx_clock_alive_toggle,
            sync => tx_clock_alive_sys
        );

    U_tx_reset_status_sync : entity work.synchronizer
        generic map (RESET_LEVEL => '0')
        port map (
            reset => sys_reset,
            clock => sys_clock,
            async => tx_reset,
            sync => tx_reset_sys
        );

    process(sys_clock, sys_reset)
    begin
        if sys_reset = '1' then
            state <= IDLE;
            reset_hold <= '0';
            busy_i <= '0';
            done_i <= '0';
            error_i <= '0';
            released_i <= '0';
            tx_clock_alive_seen <= '0';
            tx_clock_edges_after_release <= 0;
            tx_clock_baseline_wait <= 0;
        elsif rising_edge(sys_clock) then
            if tx_reset_sys = '1' then
                -- The TX-domain reset (normally an FX3 SYS_RST consequence)
                -- invalidates any prior empty/release certificate. Preserve
                -- an active fail-closed barrier across it; a half-released
                -- operation returns to HELD and must be explicitly retried.
                released_i <= '0';
                if state = WAIT_TX_CLOCK_BASELINE or
                   state = WAIT_TX_CLOCK or state = RELEASE_WAIT then
                    busy_i <= '1';
                    done_i <= '1';
                    reset_hold <= '1';
                    state <= HELD;
                    tx_clock_edges_after_release <= 0;
                    tx_clock_baseline_wait <= 0;
                end if;
            else
                if start = '1' and state /= IDLE then
                    error_i <= '1';
                end if;
                case state is
                when IDLE =>
                    busy_i <= '0';
                    done_i <= '0';
                    reset_hold <= '0';
                    if start = '1' then
                        -- This command is the explicit NIOS fault-recovery
                        -- authorization. It is not connected to normal retune
                        -- ARM/COMPLETE and cannot flush a scheduled queue.
                        busy_i <= '1';
                        done_i <= '0';
                        error_i <= '0';
                        released_i <= '0';
                        reset_hold <= '1';
                        state <= ASSERT_CLEAR;
                    elsif release_request = '1' then
                        error_i <= '1';
                    end if;

                when ASSERT_CLEAR =>
                    busy_i <= '1';
                    reset_hold <= '1';
                    if write_block_seen_sys = '1' and
                       write_empty_ready_sys = '0' and
                       read_empty_ready_sys = '0' and gpif_idle = '1' then
                        -- PCLK has sampled the gate and GPIF has finished its
                        -- current TX transaction while refusing new ones.
                        -- Both ready synchronizers have also observed clear,
                        -- preventing a prior transaction's empty level from
                        -- being mistaken for post-clear evidence.
                        done_i <= '1';
                        state <= HELD;
                    end if;

                when HELD =>
                    busy_i <= '1';
                    done_i <= '1';
                    reset_hold <= '1';
                    if release_request = '1' then
                        -- Host requests only after FX3 DMA reset/rearm and
                        -- RFIC initialization succeeded. Require fresh clock
                        -- edges here before deasserting asynchronous clear.
                        tx_clock_edges_after_release <= 0;
                        tx_clock_baseline_wait <= 0;
                        state <= WAIT_TX_CLOCK_BASELINE;
                    end if;

                when WAIT_TX_CLOCK_BASELINE =>
                    busy_i <= '1';
                    done_i <= '1';
                    reset_hold <= '1';
                    -- Let the two-stage CDC synchronizer drain edges already
                    -- in flight when the host release command arrived.
                    if tx_clock_baseline_wait = 2 then
                        tx_clock_alive_seen <= tx_clock_alive_sys;
                        tx_clock_edges_after_release <= 0;
                        state <= WAIT_TX_CLOCK;
                    else
                        tx_clock_baseline_wait <= tx_clock_baseline_wait + 1;
                    end if;

                when WAIT_TX_CLOCK =>
                    busy_i <= '1';
                    done_i <= '1';
                    reset_hold <= '1';
                    if tx_clock_alive_sys /= tx_clock_alive_seen then
                        tx_clock_alive_seen <= tx_clock_alive_sys;
                        if tx_clock_edges_after_release = 2 then
                            tx_clock_edges_after_release <= 3;
                            reset_hold <= '0';
                            state <= RELEASE_WAIT;
                        else
                            tx_clock_edges_after_release <=
                                tx_clock_edges_after_release + 1;
                        end if;
                    end if;

                when RELEASE_WAIT =>
                    busy_i <= '1';
                    done_i <= '1';
                    reset_hold <= '0';
                    if write_empty_ready_sys = '1' and
                       read_empty_ready_sys = '1' then
                        busy_i <= '0';
                        done_i <= '0';
                        released_i <= '1';
                        state <= IDLE;
                    end if;
                end case;
            end if;
        end if;
    end process;
end architecture;
