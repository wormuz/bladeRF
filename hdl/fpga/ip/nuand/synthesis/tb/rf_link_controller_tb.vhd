library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity rf_link_controller_tb is
end entity;

architecture tb of rf_link_controller_tb is

    constant CLOCK_HALF_PERIOD : time := 5 ns;

    signal clock              : std_logic := '0';
    signal reset              : std_logic := '1';
    signal cfg_word           : std_logic_vector(31 downto 0) := (others => '0');
    signal usb_speed_live     : std_logic := '0';

    signal start_toggle_out   : std_logic;
    signal stop_toggle_out    : std_logic;
    signal clear_fault_out    : std_logic;
    signal requested_speed    : std_logic;
    signal start_speed_disagreement : std_logic;
    signal host_epoch_tag     : std_logic_vector(7 downto 0);

    signal stop_sim : boolean := false;

begin

    clock <= not clock after CLOCK_HALF_PERIOD when not stop_sim else clock;

    dut : entity work.rf_link_controller
        port map (
            clock                     => clock,
            reset                     => reset,
            cfg_word                  => cfg_word,
            usb_speed_live            => usb_speed_live,
            start_toggle_out          => start_toggle_out,
            stop_toggle_out           => stop_toggle_out,
            clear_fault_out           => clear_fault_out,
            requested_speed           => requested_speed,
            start_speed_disagreement  => start_speed_disagreement,
            host_epoch_tag            => host_epoch_tag
        );

    stimulus : process
        variable start_before : std_logic;
        variable stop_before  : std_logic;
        variable tag_before   : std_logic_vector(7 downto 0);
    begin
        -- Reset
        reset <= '1';
        wait for 3 * CLOCK_HALF_PERIOD * 2;
        wait until rising_edge(clock);
        reset <= '0';
        wait until rising_edge(clock);

        ----------------------------------------------------------------
        -- Case 1: repeated identical cfg_word write -> no start pulse,
        -- epoch (host_epoch_tag) does not advance.
        -- All toggle bits start at '0' (reset value); write a word with
        -- every toggle bit still '0' (only the tag field is non-zero),
        -- so nothing changes on the first write, then repeat it.
        ----------------------------------------------------------------
        cfg_word <= x"00000200";  -- tag=0x02, speed=0, start=0, stop=0, clear=0 (no toggle change vs reset)
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        start_before := start_toggle_out;
        tag_before   := host_epoch_tag;

        -- Re-write the exact same word again: no toggle bit changed.
        cfg_word <= x"00000200";
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        assert start_toggle_out = start_before
            report "FAIL case1: repeated identical write produced a start pulse"
            severity error;
        assert host_epoch_tag = tag_before
            report "FAIL case1: epoch tag changed on identical repeated write"
            severity error;

        ----------------------------------------------------------------
        -- Case 2: start toggle with requested_speed == usb_speed_live
        -- -> start_toggle_out changes.
        -- current start bit = '0'; flip to '1' with speed bit matching
        -- usb_speed_live ('1').
        ----------------------------------------------------------------
        usb_speed_live <= '1';
        wait until rising_edge(clock);
        start_before := start_toggle_out;
        cfg_word <= x"00000303";  -- tag=0x03, speed=1, start=1 (0->1 change), stop=0, clear=0
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        assert start_toggle_out /= start_before
            report "FAIL case2: matching speed start did not toggle start_toggle_out"
            severity error;
        assert start_speed_disagreement = '0'
            report "FAIL case2: disagreement flagged despite matching speed"
            severity error;

        ----------------------------------------------------------------
        -- Case 3: start toggle with requested_speed /= usb_speed_live
        -- -> start_toggle_out does NOT change, disagreement = '1'.
        -- current start bit = '1'; flip to '0' (still a change -> pulse),
        -- with speed bit = '1' while usb_speed_live is driven to '0'.
        ----------------------------------------------------------------
        usb_speed_live <= '0';
        wait until rising_edge(clock);
        start_before := start_toggle_out;
        cfg_word <= x"00000001";  -- tag=0, speed=1 (mismatched vs live '0'), start=0 (1->0 change), stop=0, clear=0
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        assert start_toggle_out = start_before
            report "FAIL case3: mismatched speed start incorrectly toggled start_toggle_out"
            severity error;
        assert start_speed_disagreement = '1'
            report "FAIL case3: disagreement not set on speed mismatch"
            severity error;

        ----------------------------------------------------------------
        -- Case 4: clear-fault toggle after a disagreement -> disagreement
        -- returns to '0'.
        -- current clear-fault bit = '0'; flip to '1'.
        ----------------------------------------------------------------
        cfg_word <= x"00000009";  -- speed=1, start=0 (no change), stop=0, clear=1 (0->1 change)
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        assert start_speed_disagreement = '0'
            report "FAIL case4: clear-fault pulse did not clear sticky disagreement"
            severity error;

        ----------------------------------------------------------------
        -- Case 5: stop toggle -> stop_toggle_out changes regardless of
        -- speed agreement. current stop bit = '0'; flip to '1' with
        -- speed still mismatched against usb_speed_live ('0').
        ----------------------------------------------------------------
        stop_before := stop_toggle_out;
        cfg_word <= x"0000000D";  -- speed=1 (mismatched vs live '0'), start=0, stop=1 (0->1 change), clear=1 (no change)
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        assert stop_toggle_out /= stop_before
            report "FAIL case5: stop toggle did not change stop_toggle_out"
            severity error;

        ----------------------------------------------------------------
        -- Case 6: clear-fault and a mismatched start arriving in the SAME
        -- write. The clear forgives what came before it; it must not
        -- swallow the disagreement this very write causes. Otherwise the
        -- host reads "no faults" while the link silently failed to start,
        -- which is the worst of both answers.
        --
        -- Current state after case 5: stop=1, clear=1. Flip both start and
        -- clear in one word, with speed still mismatched.
        ----------------------------------------------------------------
        -- Settle to a known word first: speed=1 (mismatched), start=0,
        -- stop=1, clear=1. Only bit 1 and bit 3 move in the next write.
        cfg_word <= x"0000000D";
        wait until rising_edge(clock);
        wait until rising_edge(clock);

        start_before := start_toggle_out;
        -- speed=1, start 0->1 (edge), stop unchanged, clear 1->0 (edge).
        cfg_word <= x"00000007";
        wait until rising_edge(clock);
        wait until rising_edge(clock);
        assert start_speed_disagreement = '1'
            report "FAIL case6: same-cycle clear masked a fresh disagreement"
            severity error;
        assert start_toggle_out = start_before
            report "FAIL case6: mismatched start was forwarded"
            severity error;

        report "rf_link_controller_tb: all assertions passed";
        stop_sim <= true;
        wait;
    end process;

end architecture;
