library ieee;
    use ieee.std_logic_1164.all;
    use std.env.all;

entity tx_fifo_fault_flush_tb is
end entity;

architecture test of tx_fifo_fault_flush_tb is
    signal sys_clock         : std_logic := '0';
    signal sys_reset         : std_logic := '1';
    signal tx_clock          : std_logic := '0';
    signal tx_clock_enable   : std_logic := '0';
    signal write_clock       : std_logic := '0';
    signal tx_reset          : std_logic := '1';
    signal start             : std_logic := '0';
    signal release_cmd       : std_logic := '0';
    signal gpif_idle         : std_logic := '1';
    signal fifo_aclr         : std_logic;
    signal write_block       : std_logic;
    signal reader_block      : std_logic;
    signal busy              : std_logic;
    signal done              : std_logic;
    signal error             : std_logic;
    signal empty_confirmed   : std_logic;
    signal release_complete  : std_logic;
    signal write_drain_ready : std_logic;
    signal inject_stale      : std_logic := '0';
    signal stale_entries     : std_logic := '0';
    signal sample_wempty     : std_logic := '0';
    signal meta_wempty       : std_logic := '0';
    signal sample_rempty     : std_logic := '0';
    signal meta_rempty       : std_logic := '0';
begin
    sys_clock <= not sys_clock after 5 ns;
    tx_clock <= not tx_clock after 5 ns when tx_clock_enable = '1' else '0';
    write_clock <= not write_clock after 7 ns;

    dut : entity work.tx_fifo_fault_flush
        port map (
            sys_clock => sys_clock,
            sys_reset => sys_reset,
            tx_clock => tx_clock,
            tx_reset => tx_reset,
            write_clock => write_clock,
            start => start,
            release_request => release_cmd,
            gpif_idle => gpif_idle,
            sample_wempty => sample_wempty,
            meta_wempty => meta_wempty,
            sample_rempty => sample_rempty,
            meta_rempty => meta_rempty,
            fifo_aclr => fifo_aclr,
            write_block => write_block,
            reader_block => reader_block,
            busy => busy,
            done => done,
            error => error,
            empty_confirmed => empty_confirmed,
            release_complete => release_complete,
            write_drain_ready => write_drain_ready
        );

    fifo_contents : process(fifo_aclr, inject_stale)
    begin
        if fifo_aclr = '1' then
            stale_entries <= '0';
        elsif inject_stale = '1' then
            stale_entries <= '1';
        end if;
    end process;

    -- Model DCFIFO flags: clear asserts empty asynchronously, while each
    -- domain's release pipeline needs three local edges before empty is valid.
    write_flags : process(write_clock, fifo_aclr)
        variable release_edges : natural range 0 to 3 := 0;
    begin
        if fifo_aclr = '1' then
            release_edges := 0;
            sample_wempty <= '1';
            meta_wempty <= '1';
        elsif rising_edge(write_clock) then
            if release_edges < 3 then
                release_edges := release_edges + 1;
            else
                sample_wempty <= not stale_entries;
                meta_wempty <= not stale_entries;
            end if;
        end if;
    end process;

    read_flags : process(tx_clock, fifo_aclr)
        variable release_edges : natural range 0 to 3 := 0;
    begin
        if fifo_aclr = '1' then
            release_edges := 0;
            sample_rempty <= '1';
            meta_rempty <= '1';
        elsif rising_edge(tx_clock) then
            if release_edges < 3 then
                release_edges := release_edges + 1;
            else
                sample_rempty <= not stale_entries;
                meta_rempty <= not stale_entries;
            end if;
        end if;
    end process;

    stimulus : process
    begin
        wait for 40 ns;
        sys_reset <= '0';
        tx_reset <= '0';
        wait for 40 ns;

        inject_stale <= '1';
        wait for 20 ns;
        inject_stale <= '0';

        -- The AD9361 TX clock is stopped when the fault-only command arrives.
        start <= '1';
        wait until rising_edge(sys_clock);
        start <= '0';
        wait until done = '1' and write_drain_ready = '1' for 1 us;
        assert write_drain_ready = '1' and busy = '1' and done = '1'
            report "system-clock flush did not establish independent PCLK drain"
            severity failure;
        assert fifo_aclr = '1' and write_block = '1' and reader_block = '1'
            report "fault barrier did not hold FIFO clear and data gates"
            severity failure;
        assert empty_confirmed = '0' and release_complete = '0'
            report "stopped TX clock was falsely certified as cleared"
            severity failure;

        -- Even an early release request must not remove ACLR without fresh
        -- TX-clock edges. In production the host sends it after full RFIC init.
        release_cmd <= '1';
        wait until rising_edge(sys_clock);
        release_cmd <= '0';
        wait for 100 ns;
        assert fifo_aclr = '1' and busy = '1' and
               release_complete = '0' and empty_confirmed = '0'
            report "clear released without proof that TX clock returned"
            severity failure;

        tx_clock_enable <= '1';
        wait until release_complete = '1' for 1 us;
        wait until write_block = '0' and reader_block = '0' for 1 us;
        assert release_complete = '1' and empty_confirmed = '1' and
               busy = '0' and write_block = '0' and reader_block = '0'
            report "recovered TX clock did not complete both-domain empty proof"
            severity failure;
        assert sample_wempty = '1' and meta_wempty = '1' and
               sample_rempty = '1' and meta_rempty = '1'
            report "stale sample or metadata entries remain after recovery"
            severity failure;

        report "tx_fifo_fault_flush_tb: PASS" severity note;
        stop;
    end process;
end architecture;
