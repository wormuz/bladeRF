-- T8 (doc/development/rx-spatial-mimo-fir-design.md 6.1): rx_spatial_fir
-- controller CDC with the real work.handshake, unrelated sys/rx clocks.
--
-- The fabric side is wired as in bladerf_core: controller -> rx_spatial_fir
-- bank ports, and the status word back through a second handshake into a
-- sys_clock capture register. A NIOS model drives the two output PIOs with
-- the firmware order (data first, then ctrl) and waits for the echo in the
-- status word with a bounded poll, as pkt_8x32/devices_inline.h do.
--
-- Checked:
--   A  20 back-to-back writes + commit: every write lands once, in order,
--      with its data; one commit pulse with the tag; status tag/identity.
--   B  force_identity level: applied (status bit 7) and bypass follows.
--   C  reset in the middle: a write toggled just before SYS_RST and the
--      surviving toggle levels are adopted without any pulse; bank back to
--      identity, tag 0; the surviving force level is applied after init.
--   D  after reset, 20 writes + commit with force cleared in the same ctrl
--      write: all land, force drops, bank active.
--   E  one ctrl write carrying both a write and a commit edge: the write
--      reaches the shadow before the commit takes it.
--   F  identity bank written word by word + commit: status identity bit.
-- Run with RX_HALF_PS = 4069 (122.88 MHz, faster than sys) and 11000
-- (slower than sys) by host/misc/run_rx_spatial_fir_tb.sh.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity rx_spatial_fir_controller_tb is
    generic (
        RX_HALF_PS : natural := 4069
    );
end entity;

architecture sim of rx_spatial_fir_controller_tb is

    constant SYS_HALF   : time := 6.25 ns;   -- 80 MHz sys_clock
    constant RX_HALF    : time := RX_HALF_PS * 1 ps;
    constant POLL_LIMIT : natural := 4000;   -- sys cycles per echo wait
    constant NCOEF      : natural := 20;

    type word_arr_t is array (natural range <>) of std_logic_vector(31 downto 0);

    signal sys_clock  : std_logic := '0';
    signal rx_clock   : std_logic := '0';
    signal sys_reset  : std_logic := '1';
    signal rx_reset   : std_logic := '1';
    signal done       : boolean := false;

    signal pio_data   : std_logic_vector(31 downto 0) := (others => '0');
    signal pio_ctrl   : std_logic_vector(31 downto 0) := (others => '0');

    signal wr_en      : std_logic;
    signal wr_idx     : unsigned(4 downto 0);
    signal wr_data    : std_logic_vector(31 downto 0);
    signal commit     : std_logic;
    signal commit_tag : unsigned(7 downto 0);
    signal force_lvl      : std_logic;
    signal status_ctl : std_logic_vector(7 downto 0);

    signal ctl        : sample_controls_t(0 to 1) := (others => SAMPLE_CONTROL_ENABLE);
    signal adc        : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal act_tag    : unsigned(7 downto 0);
    signal bypass     : std_logic;
    signal act_id     : std_logic;
    signal both       : std_logic;
    signal sat        : std_logic_vector(1 downto 0);

    signal status_rx  : std_logic_vector(31 downto 0);
    signal st_wire    : std_logic_vector(31 downto 0);
    signal st_req     : std_logic := '0';
    signal st_ack     : std_logic;
    signal status_sys : std_logic_vector(31 downto 0) := (others => '0');

    -- rx-side observation (written by the monitor process only)
    signal wr_count     : natural := 0;
    signal commit_count : natural := 0;
    signal wr_log       : word_arr_t(0 to 255) := (others => (others => '0'));
    signal idx_log      : word_arr_t(0 to 255) := (others => (others => '0'));
    signal last_tag     : unsigned(7 downto 0) := (others => '0');
    -- TB model of the datapath banks, same commit/write ordering rule.
    signal m_shadow     : word_arr_t(0 to NCOEF-1) := (others => (others => '0'));
    signal m_active     : word_arr_t(0 to NCOEF-1) := (others => (others => '0'));

    function lfsr_next(x : unsigned(31 downto 0)) return unsigned is
    begin
        return x(30 downto 0) & (x(31) xor x(21) xor x(1) xor x(0));
    end function;

    function identity_word(idx : natural) return std_logic_vector is
    begin
        if idx = 2 or idx = 17 then
            return x"00004000";
        end if;
        return x"00000000";
    end function;
begin

    sys_clock <= not sys_clock after SYS_HALF when not done;
    rx_clock  <= not rx_clock after RX_HALF when not done;

    U_ctrl : entity work.rx_spatial_fir_controller
        port map (
            source_clock    => sys_clock,
            source_reset    => sys_reset,
            source_data     => pio_data,
            source_ctrl     => pio_ctrl,
            rx_clock        => rx_clock,
            rx_reset        => rx_reset,
            coef_wr_en      => wr_en,
            coef_wr_idx     => wr_idx,
            coef_wr_data    => wr_data,
            coef_commit     => commit,
            coef_commit_tag => commit_tag,
            force_identity  => force_lvl,
            status_ctrl     => status_ctl
        );

    U_fir : entity work.rx_spatial_fir
        port map (
            clock => rx_clock, reset => rx_reset,
            in_sample_controls => ctl, in_samples => adc,
            rx_timestamp => (others => '0'),
            coef_wr_en => wr_en, coef_wr_idx => wr_idx, coef_wr_data => wr_data,
            coef_commit => commit, coef_commit_tag => commit_tag,
            force_identity => force_lvl,
            out_samples => open, out_timestamp => open,
            out_epoch_complete => open, out_support_clean => open,
            out_active_tag => act_tag, out_bypass => bypass,
            out_active_identity => act_id, out_both_lanes => both,
            out_sat_sticky => sat
        );

    -- Status word as composed in bladerf_core.vhd.
    status_rx <= x"1" & "0000000" & sat & both & act_id & bypass &
                 std_logic_vector(act_tag) & status_ctl;

    U_status : entity work.handshake
        generic map (DATA_WIDTH => 32)
        port map (
            source_reset => rx_reset, source_clock => rx_clock,
            source_data => status_rx,
            dest_reset => sys_reset, dest_clock => sys_clock,
            dest_data => st_wire, dest_req => st_req, dest_ack => st_ack
        );

    st_drive : process(sys_clock, sys_reset)
    begin
        if sys_reset = '1' then
            st_req <= '0';
            status_sys <= (others => '0');
        elsif rising_edge(sys_clock) then
            st_req <= not st_ack;
            if st_ack = '1' then
                status_sys <= st_wire;
            end if;
        end if;
    end process;

    -- Keep data_v toggling so the datapath sees the hardware cadence.
    stim : process(rx_clock)
        variable t : std_logic := '0';
    begin
        if rising_edge(rx_clock) then
            t := not t;
            for j in 0 to 1 loop
                adc(j).data_v <= t;
                adc(j).data_i <= to_signed(100 + j, 16);
                adc(j).data_q <= to_signed(-50 - j, 16);
            end loop;
        end if;
    end process;

    monitor : process(rx_clock)
        variable n : natural;
    begin
        if rising_edge(rx_clock) then
            if rx_reset = '1' then
                for m in 0 to NCOEF-1 loop
                    m_shadow(m) <= identity_word(m);
                    m_active(m) <= identity_word(m);
                end loop;
            else
                if commit = '1' then
                    commit_count <= commit_count + 1;
                    last_tag <= commit_tag;
                    m_active <= m_shadow;
                end if;
                if wr_en = '1' then
                    n := wr_count;
                    wr_log(n)  <= wr_data;
                    idx_log(n) <= std_logic_vector(resize(wr_idx, 32));
                    wr_count   <= n + 1;
                    if to_integer(wr_idx) < NCOEF then
                        m_shadow(to_integer(wr_idx)) <= wr_data;
                    end if;
                end if;
            end if;
        end if;
    end process;

    main : process
        variable lfsr      : unsigned(31 downto 0) := x"6B8B4567";
        variable ctrl_word : std_logic_vector(31 downto 0) := (others => '0');
        variable words     : word_arr_t(0 to NCOEF-1);
        variable base_wr   : natural;
        variable base_cm   : natural;
        variable errors    : natural := 0;

        procedure sys_wait(n : natural) is
        begin
            for i in 1 to n loop
                wait until rising_edge(sys_clock);
            end loop;
        end procedure;

        procedure check(cond : boolean; msg : string) is
        begin
            if not cond then
                report "T8 FAIL: " & msg severity error;
                errors := errors + 1;
            end if;
        end procedure;

        -- Bounded poll of the sys-side status for (status and mask) = value.
        procedure wait_status(mask, value : std_logic_vector(31 downto 0);
                              msg : string) is
        begin
            for i in 1 to POLL_LIMIT loop
                wait until rising_edge(sys_clock);
                if (status_sys and mask) = value then
                    return;
                end if;
            end loop;
            check(false, "echo timeout: " & msg);
        end procedure;

        procedure nios_write(idx : natural; w : std_logic_vector(31 downto 0);
                             wait_echo : boolean := true) is
        begin
            pio_data <= w;
            wait until rising_edge(sys_clock);
            ctrl_word(4 downto 0) := std_logic_vector(to_unsigned(idx, 5));
            ctrl_word(5) := not ctrl_word(5);
            pio_ctrl <= ctrl_word;
            if wait_echo then
                wait_status(x"0000003F", x"000000" & "00" & ctrl_word(5 downto 0),
                            "write " & integer'image(idx));
            end if;
        end procedure;

        procedure nios_commit(tag : natural; clear_force : boolean) is
        begin
            ctrl_word(23 downto 16) := std_logic_vector(to_unsigned(tag, 8));
            ctrl_word(6) := not ctrl_word(6);
            if clear_force then
                ctrl_word(7) := '0';
            end if;
            pio_ctrl <= ctrl_word;
            wait_status(x"0000FF40",
                        x"0000" & ctrl_word(23 downto 16) & '0' & ctrl_word(6) & "000000",
                        "commit " & integer'image(tag));
        end procedure;

        procedure nios_force(level : std_logic) is
        begin
            ctrl_word(7) := level;
            pio_ctrl <= ctrl_word;
        end procedure;

        procedure rand_bank is
        begin
            for m in 0 to NCOEF-1 loop
                lfsr := lfsr_next(lfsr);
                lfsr := lfsr_next(lfsr);
                words(m) := std_logic_vector(lfsr);
            end loop;
            -- Never identity: the first word is non-zero off-centre.
            words(0)(15 downto 0) := x"1234";
        end procedure;

        procedure write_bank_and_check(tag : natural; clear_force : boolean;
                                       name : string) is
        begin
            base_wr := wr_count;
            base_cm := commit_count;
            for m in 0 to NCOEF-1 loop
                nios_write(m, words(m));
            end loop;
            nios_commit(tag, clear_force);
            sys_wait(4);
            check(wr_count = base_wr + NCOEF,
                  name & ": write pulses " & integer'image(wr_count - base_wr));
            check(commit_count = base_cm + 1,
                  name & ": commit pulses " & integer'image(commit_count - base_cm));
            for m in 0 to NCOEF-1 loop
                check(wr_log(base_wr + m) = words(m), name & ": data of write " & integer'image(m));
                check(unsigned(idx_log(base_wr + m)) = m, name & ": index of write " & integer'image(m));
                check(m_active(m) = words(m), name & ": active word " & integer'image(m));
            end loop;
            check(last_tag = tag, name & ": commit tag");
        end procedure;
    begin
        pio_ctrl <= (others => '0');
        sys_wait(5);
        sys_reset <= '0';
        wait for 33 ns;
        rx_reset <= '0';
        sys_wait(50);

        -- A: back-to-back writes, commit
        rand_bank;
        write_bank_and_check(16#5A#, false, "A");
        wait_status(x"00070000", x"00040000", "A status: not identity, not bypass, both lanes");

        -- B: force_identity level
        nios_force('1');
        wait_status(x"00010080", x"00010080", "B force applied, bypass");
        check(force_lvl = '1', "B force level at the datapath");
        nios_force('0');
        wait_status(x"00010080", x"00000000", "B force cleared, FIR active");

        -- C: reset in the middle. After A the commit toggle is 1; the write
        -- below (21st, no echo wait) leaves the write toggle at 1. Force set,
        -- then pulse SYS_RST right after that write
        -- (both domains; the NIOS PIO is not reset).
        nios_force('1');
        sys_wait(2);
        base_wr := wr_count;
        base_cm := commit_count;
        nios_write(4, x"DEADBEEF", wait_echo => false);
        sys_reset <= '1';
        wait for 7 ns;
        rx_reset <= '1';
        sys_wait(20);
        sys_reset <= '0';
        wait for 21 ns;
        rx_reset <= '0';
        -- Echo of the surviving levels after the first capture.
        wait_status(x"000000FF", x"000000" & ctrl_word(7 downto 0), "C adopted levels");
        sys_wait(200);
        check(wr_count = base_wr, "C: reset fired a write pulse");
        check(commit_count = base_cm, "C: reset fired a commit pulse");
        wait_status(x"0003FF00", x"00030000", "C bank identity, tag 0, bypass");
        check(force_lvl = '1', "C surviving force level applied after init");

        -- D: full load after reset, force cleared together with the commit.
        rand_bank;
        write_bank_and_check(16#A5#, true, "D");
        wait_status(x"0003FF80", x"0000A500", "D status");
        check(force_lvl = '0', "D force cleared");

        -- E: one ctrl write with a write edge and a commit edge.
        base_wr := wr_count;
        base_cm := commit_count;
        pio_data <= x"77776666";
        sys_wait(1);
        ctrl_word(4 downto 0) := "00101";
        ctrl_word(5) := not ctrl_word(5);
        ctrl_word(6) := not ctrl_word(6);
        ctrl_word(23 downto 16) := x"E1";
        pio_ctrl <= ctrl_word;
        wait_status(x"0000FF7F", x"0000E1" & '0' & ctrl_word(6 downto 0), "E echo");
        sys_wait(4);
        check(wr_count = base_wr + 1 and commit_count = base_cm + 1, "E pulse counts");
        check(m_active(5) = x"77776666", "E commit took the word written with it");

        -- F: identity bank, word by word, then commit.
        for m in 0 to NCOEF-1 loop
            words(m) := identity_word(m);
        end loop;
        write_bank_and_check(16#11#, false, "F");
        wait_status(x"0003FF00", x"00031100", "F identity bit, bypass, tag");

        if errors = 0 then
            report "rx_spatial_fir_controller_tb: PASS (RX_HALF_PS=" &
                   integer'image(RX_HALF_PS) & ")" severity note;
        else
            report "rx_spatial_fir_controller_tb: " & integer'image(errors) &
                   " failures" severity failure;
        end if;
        done <= true;
        wait;
    end process;

end architecture;
