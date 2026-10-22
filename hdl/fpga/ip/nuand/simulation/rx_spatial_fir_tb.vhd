-- rx_spatial_fir bit-exact testbench against vectors from the golden model
-- (rx_spatial_fir_vectors.py -> sdr-scanner rfcore/spatial_fir.apply_fixed).
-- Covers design 6.1 T1 (identity / force_identity), T3 (random and fitted
-- coefficients), T4 (saturation, sticky), T5 (commit atomicity), T6 (RX_X1
-- bypass, enable toggled). Every test also checks: latency 9 cycles for every
-- sample, per-lane output valid, and the carried timestamp = ts_in(n-2).
--
-- STROBE: 0 = data_v every 2nd clock (rx_clock = 2 x fs), 1 = every clock,
--         2 = irregular (LFSR, includes back-to-back strobes and gaps).

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library std;
    use std.textio.all;

library work;
    use work.fifo_readwrite_p.all;

entity rx_spatial_fir_tb is
    generic (
        VEC_DIR : string  := "rx_spatial_fir_vectors";
        STROBE  : natural := 0;
        -- Subset of TESTS (index range). The first six exercise the control
        -- path (commit, enables, force, sticky, RX_X1 strobe); the run script
        -- uses them for STROBE 1 and 2, the full list for STROBE 0.
        FIRST   : natural := 0;
        LAST    : natural := 9
    );
end entity;

architecture sim of rx_spatial_fir_tb is

    constant NMAX    : natural := 600;
    constant KMAX    : natural := 3;
    constant LATENCY : natural := 9;

    type name_list_t is array (natural range <>) of string(1 to 14);
    constant TESTS : name_list_t := (
        "t1_ident_reset", "t1_force      ", "t5_commit     ", "t6_toggle     ",
        "t6_x1_lane1   ", "t4_sat_clear  ", "t3_rand       ", "t3_fit        ",
        "t4_sat        ", "t6_x1_lane0   ");

    type int_arr_t is array (natural range <>) of integer;
    type ts_arr_t  is array (natural range <>) of unsigned(63 downto 0);

    function trim(s : string) return string is
    begin
        for n in s'range loop
            if s(n) = ' ' then
                return s(s'low to n - 1);
            end if;
        end loop;
        return s;
    end function;

    signal clock   : std_logic := '0';
    signal reset   : std_logic := '1';
    signal done    : boolean := false;

    signal ctrl    : sample_controls_t(0 to 1) := (others => SAMPLE_CONTROL_ENABLE);
    signal din     : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal ts      : unsigned(63 downto 0) := (others => '0');
    signal wr_en   : std_logic := '0';
    signal wr_idx  : unsigned(4 downto 0) := (others => '0');
    signal wr_data : std_logic_vector(31 downto 0) := (others => '0');
    signal commit  : std_logic := '0';
    signal tag     : unsigned(7 downto 0) := (others => '0');
    signal force_id   : std_logic := '0';

    signal dout    : sample_streams_t(0 to 1);
    signal dout_ts : unsigned(63 downto 0);
    signal st_tag  : unsigned(7 downto 0);
    signal st_byp, st_ident, st_both : std_logic;
    signal st_sat  : std_logic_vector(1 downto 0);
begin

    clock <= not clock after 4 ns when not done;

    dut : entity work.rx_spatial_fir
        port map (
            clock => clock, reset => reset,
            in_sample_controls => ctrl, in_samples => din, rx_timestamp => ts,
            epoch_arm => '0', epoch_complete => '0', epoch_abort => '0',
            coef_wr_en => wr_en, coef_wr_idx => wr_idx, coef_wr_data => wr_data,
            coef_commit => commit, coef_commit_tag => tag, force_identity => force_id,
            out_samples => dout, out_timestamp => dout_ts,
            out_epoch_complete => open, out_support_clean => open,
            out_active_tag => st_tag, out_bypass => st_byp,
            out_active_identity => st_ident, out_both_lanes => st_both,
            out_sat_sticky => st_sat
        );

    stim : process
        file f        : text;
        variable l    : line;
        variable n_s, k_c, nseq, nev, init_bank, e0, e1, frc : integer;
        variable exp_sat, exp_tag, exp_ident : integer;
        variable seq  : int_arr_t(0 to 7);
        variable ev   : int_arr_t(0 to 4 * 8 - 1);
        variable ev_done : boolean_vector(0 to 7);
        variable x    : int_arr_t(0 to 4 * NMAX - 1);
        variable g    : int_arr_t(0 to 4 * KMAX * NMAX - 1);
        variable bank : int_arr_t(0 to 39);
        variable bank_file : integer;
        variable bank_pos  : integer;
        variable in_cyc    : int_arr_t(0 to NMAX - 1);
        variable in_lv     : int_arr_t(0 to NMAX - 1);
        variable ts_in     : ts_arr_t(0 to NMAX - 1);
        variable en    : std_logic_vector(1 downto 0);
        variable lfsr  : std_logic_vector(15 downto 0);
        variable stb : boolean;
        variable cyc, idx, o, phase, cand : integer;
        variable last_strobe : integer;
        variable ts_v  : unsigned(63 downto 0);
        variable lv_o  : std_logic_vector(1 downto 0);
        variable ok_cur, ok_next : boolean;
        variable errors : natural := 0;
        variable total  : natural := 0;

        procedure read_bank(name : string; b : integer) is
            file fb : text;
            variable lb : line;
        begin
            file_open(fb, VEC_DIR & "/" & name & ".bank" & integer'image(b), read_mode);
            for m in 0 to 19 loop
                readline(fb, lb);
                read(lb, bank(2 * m));
                read(lb, bank(2 * m + 1));
            end loop;
            file_close(fb);
        end procedure;

        procedure bank_word(m : integer) is
        begin
            wr_en   <= '1';
            wr_idx  <= to_unsigned(m, 5);
            wr_data <= std_logic_vector(to_signed(bank(2 * m + 1), 16)) &
                       std_logic_vector(to_signed(bank(2 * m), 16));
        end procedure;

        impure function matches(c : integer; s : integer; lv : std_logic_vector(1 downto 0);
                         y : sample_streams_t) return boolean is
            variable b : integer;
        begin
            for i in 0 to 1 loop
                if lv(i) = '1' then
                    b := (s * KMAX * 4) + c * 4 + i * 2;
                    if to_integer(y(i).data_i) /= g(b) or
                       to_integer(y(i).data_q) /= g(b + 1) then
                        return false;
                    end if;
                end if;
            end loop;
            return true;
        end function;
    begin
        lfsr := x"ACE1";
        for t in FIRST to LAST loop
            -- script
            file_open(f, VEC_DIR & "/" & trim(TESTS(t)) & ".scr", read_mode);
            readline(f, l);
            read(l, n_s); read(l, k_c); read(l, nseq); read(l, nev);
            read(l, init_bank); read(l, e0); read(l, e1); read(l, frc);
            read(l, exp_sat); read(l, exp_tag); read(l, exp_ident);
            readline(f, l);
            for q in 0 to nseq - 1 loop
                read(l, seq(q));
            end loop;
            for q in 0 to nev - 1 loop
                readline(f, l);
                for r in 0 to 3 loop
                    read(l, ev(4 * q + r));
                end loop;
                ev_done(q) := false;
            end loop;
            file_close(f);
            assert n_s <= NMAX and k_c <= KMAX report "vector too large" severity failure;
            file_open(f, VEC_DIR & "/" & trim(TESTS(t)) & ".in", read_mode);
            for s in 0 to n_s - 1 loop
                readline(f, l);
                for r in 0 to 3 loop
                    read(l, x(4 * s + r));
                end loop;
            end loop;
            file_close(f);
            file_open(f, VEC_DIR & "/" & trim(TESTS(t)) & ".exp", read_mode);
            for s in 0 to n_s - 1 loop
                readline(f, l);
                for r in 0 to 4 * k_c - 1 loop
                    read(l, g(s * KMAX * 4 + r));
                end loop;
            end loop;
            file_close(f);

            -- reset, then initial bank, enables and force level
            reset <= '1';
            ctrl  <= (others => SAMPLE_CONTROL_ENABLE);
            din   <= (others => ZERO_SAMPLE);
            force_id <= '0';
            for c in 0 to 3 loop
                wait until rising_edge(clock);
            end loop;
            reset <= '0';
            wait until rising_edge(clock);
            if init_bank >= 0 then
                read_bank(trim(TESTS(t)), init_bank);
                for m in 0 to 19 loop
                    bank_word(m);
                    wait until rising_edge(clock);
                end loop;
                -- The controller commits two edges after a write.
                wr_en  <= '0';
                wait until rising_edge(clock);
                commit <= '1';
                tag    <= (others => '0');
                wait until rising_edge(clock);
                commit <= '0';
            end if;
            en := std_logic(to_unsigned(e1, 1)(0)) & std_logic(to_unsigned(e0, 1)(0));
            ctrl(0).enable <= en(0);
            ctrl(1).enable <= en(1);
            if frc = 1 then force_id <= '1'; else force_id <= '0'; end if;
            for c in 0 to 3 loop
                wait until rising_edge(clock);
            end loop;

            -- stream
            cyc := 0; idx := 0; o := 0; phase := 0;
            last_strobe := -1;
            bank_pos := 20;
            ts_v := x"00000001_FFFFFF00";
            while o < n_s loop
                assert cyc < 20 * NMAX report "stall" severity failure;
                -- outputs as of the edge just taken
                lv_o := dout(1).data_v & dout(0).data_v;
                if lv_o /= "00" then
                    assert o < n_s report "extra output" severity failure;
                    if cyc - in_cyc(o) /= LATENCY + 1 then
                        report trim(TESTS(t)) & ": latency " &
                            integer'image(cyc - in_cyc(o) - 1) & " at sample " &
                            integer'image(o) severity error;
                        errors := errors + 1;
                    end if;
                    if to_integer(unsigned(lv_o)) /= in_lv(o) then
                        report trim(TESTS(t)) & ": lane valid mismatch at " &
                            integer'image(o) severity error;
                        errors := errors + 1;
                    end if;
                    if (o >= 2 and dout_ts /= ts_in(o - 2)) or
                       (o < 2 and dout_ts /= 0) then
                        report trim(TESTS(t)) & ": timestamp mismatch at " &
                            integer'image(o) severity error;
                        errors := errors + 1;
                    end if;
                    ok_cur := matches(seq(phase), o, lv_o, dout);
                    ok_next := phase + 1 < nseq and
                               matches(seq(phase + 1), o, lv_o, dout);
                    if not ok_cur and ok_next then
                        phase := phase + 1;
                    elsif not ok_cur then
                        report trim(TESTS(t)) & ": data mismatch at sample " &
                            integer'image(o) & " phase " & integer'image(phase) &
                            " lane0 " & integer'image(to_integer(dout(0).data_i)) &
                            "," & integer'image(to_integer(dout(0).data_q)) &
                            " lane1 " & integer'image(to_integer(dout(1).data_i)) &
                            "," & integer'image(to_integer(dout(1).data_q))
                            severity error;
                        errors := errors + 1;
                    end if;
                    o := o + 1;
                end if;

                -- timestamp: STROBE 0 ticks once per 2 clocks, else every clock
                if STROBE /= 0 or cyc mod 2 = 0 then
                    ts_v := ts_v + 1;
                end if;
                ts <= ts_v;
                if last_strobe >= 0 then
                    ts_in(last_strobe) := ts_v;
                    last_strobe := -1;
                end if;

                -- strobe pattern
                case STROBE is
                    when 0 => stb := cyc mod 2 = 1;
                    when 1 => stb := true;
                    when others =>
                        lfsr := lfsr(14 downto 0) & (lfsr(15) xor lfsr(13) xor
                                lfsr(12) xor lfsr(10));
                        stb := lfsr(0) = '1' or lfsr(1) = '1';
                end case;
                stb := stb and idx < n_s;

                wr_en  <= '0';
                commit <= '0';
                if bank_pos < 20 then
                    bank_word(bank_pos);
                    bank_pos := bank_pos + 1;
                end if;
                if stb then
                    for q in 0 to nev - 1 loop
                        if not ev_done(q) and ev(4 * q) = idx then
                            ev_done(q) := true;
                            case ev(4 * q + 1) is
                                when 1 =>
                                    read_bank(trim(TESTS(t)), ev(4 * q + 2));
                                    bank_pos := 0;
                                    bank_word(0);
                                    bank_pos := 1;
                                when 2 =>
                                    commit <= '1';
                                    tag    <= to_unsigned(ev(4 * q + 2), 8);
                                when 3 =>
                                    en := std_logic(to_unsigned(ev(4 * q + 3), 1)(0)) &
                                          std_logic(to_unsigned(ev(4 * q + 2), 1)(0));
                                    ctrl(0).enable <= en(0);
                                    ctrl(1).enable <= en(1);
                                when others =>
                                    if ev(4 * q + 2) = 1 then force_id <= '1';
                                    else force_id <= '0'; end if;
                            end case;
                        end if;
                    end loop;
                    for i in 0 to 1 loop
                        din(i).data_i <= to_signed(x(4 * idx + 2 * i), 16);
                        din(i).data_q <= to_signed(x(4 * idx + 2 * i + 1), 16);
                        din(i).data_v <= en(i);
                    end loop;
                    in_cyc(idx) := cyc;
                    in_lv(idx) := to_integer(unsigned(en));
                    last_strobe := idx;
                    idx := idx + 1;
                else
                    din(0).data_v <= '0';
                    din(1).data_v <= '0';
                end if;
                wait until rising_edge(clock);
                cyc := cyc + 1;
            end loop;

            if phase /= nseq - 1 then
                report trim(TESTS(t)) & ": stopped at phase " & integer'image(phase) &
                    " of " & integer'image(nseq) severity error;
                errors := errors + 1;
            end if;
            if exp_sat >= 0 and to_integer(unsigned(st_sat)) /= exp_sat then
                report trim(TESTS(t)) & ": sat sticky " &
                    integer'image(to_integer(unsigned(st_sat))) & " expected " &
                    integer'image(exp_sat) severity error;
                errors := errors + 1;
            end if;
            if exp_tag >= 0 and to_integer(st_tag) /= exp_tag then
                report trim(TESTS(t)) & ": tag mismatch" severity error;
                errors := errors + 1;
            end if;
            if exp_ident >= 0 and (st_ident = '1') /= (exp_ident = 1) then
                report trim(TESTS(t)) & ": identity flag mismatch" severity error;
                errors := errors + 1;
            end if;
            if st_both /= (en(0) and en(1)) then
                report trim(TESTS(t)) & ": both-lanes status mismatch" severity error;
                errors := errors + 1;
            end if;
            total := total + o;
            report "rx_spatial_fir_tb STROBE=" & integer'image(STROBE) & " " &
                trim(TESTS(t)) & ": " & integer'image(o) & " samples, phases " &
                integer'image(phase + 1) & "/" & integer'image(nseq);
        end loop;

        assert errors = 0
            report "rx_spatial_fir_tb FAILED: " & integer'image(errors) & " errors"
            severity failure;
        report "rx_spatial_fir_tb STROBE=" & integer'image(STROBE) & " PASS (" &
            integer'image(total) & " samples)";
        done <= true;
        wait;
    end process;

end architecture;
