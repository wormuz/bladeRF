-- rx_spatial_fir in front of rx_epoch_gate, against the stock chain
-- (design 6.1 T2 and T7). Both chains model rx.vhd: a one-register rx_mux
-- NORMAL branch in front of rx_epoch_gate. The FIR chain registers the FIR
-- timestamp and delayed COMPLETE as mux sidebands (design 1.1); ARM and ABORT
-- go to the gate directly. rx_clock = 2 x fs: data_v every 2nd clock, the
-- timestamp ticks once per 2 clocks.
--
-- T2: identity, random int16 IQ: the (IQ, timestamp) sequence at the FIR gate
--     output equals the stock sequence for every sample after the first 2.
-- T7: COMPLETE (on and off a strobe cycle) opens the FIR gate on the first
--     support_clean sample: IQ = x[m1 + 2], first_valid_timestamp =
--     ts_in(m1 + 2), m1 = first input at/after COMPLETE (the stock gate
--     admits x[m1], checked too). ARM or ABORT 1..30 cycles after COMPLETE:
--     no epoch start at the FIR gate after it; a later ARM + COMPLETE opens.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity rx_spatial_fir_gate_equiv_tb is
end entity;

architecture sim of rx_spatial_fir_gate_equiv_tb is

    constant NMAX : natural := 4096;
    type int_arr_t is array (natural range <>) of integer;
    type ts_arr_t  is array (natural range <>) of unsigned(63 downto 0);

    signal clock    : std_logic := '0';
    signal reset    : std_logic := '1';
    signal done     : boolean := false;

    signal ctrl     : sample_controls_t(0 to 1) := (others => SAMPLE_CONTROL_ENABLE);
    signal adc      : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal ts       : unsigned(63 downto 0) := (others => '0');
    signal arm, complete, abort : std_logic := '0';
    signal epoch_id : unsigned(7 downto 0) := (others => '0');

    signal fir_out  : sample_streams_t(0 to 1);
    signal fir_ts   : unsigned(63 downto 0);
    signal fir_cpl  : std_logic;

    signal f_mux, s_mux     : sample_streams_t(0 to 1) := (others => ZERO_SAMPLE);
    signal f_mux_ts         : unsigned(63 downto 0) := (others => '0');
    signal f_mux_cpl        : std_logic := '0';

    signal f_out, s_out     : sample_streams_t(0 to 1);
    signal f_ts, s_ts       : unsigned(63 downto 0);
    signal f_start, s_start : std_logic;
    signal f_first, s_first : unsigned(63 downto 0);
begin

    clock <= not clock after 4 ns when not done;

    U_fir : entity work.rx_spatial_fir
        port map (
            clock => clock, reset => reset,
            in_sample_controls => ctrl, in_samples => adc, rx_timestamp => ts,
            epoch_arm => arm, epoch_complete => complete, epoch_abort => abort,
            out_samples => fir_out, out_timestamp => fir_ts,
            out_epoch_complete => fir_cpl, out_support_clean => open,
            out_active_tag => open, out_bypass => open,
            out_active_identity => open, out_both_lanes => open,
            out_sat_sticky => open
        );

    -- rx_mux NORMAL branch with the design 1.1 sidebands, and the stock one.
    mux : process(clock, reset)
    begin
        if reset = '1' then
            f_mux <= (others => ZERO_SAMPLE);
            s_mux <= (others => ZERO_SAMPLE);
            f_mux_ts <= (others => '0');
            f_mux_cpl <= '0';
        elsif rising_edge(clock) then
            f_mux <= fir_out;
            f_mux_ts <= fir_ts;
            f_mux_cpl <= fir_cpl;
            s_mux <= adc;
        end if;
    end process;

    U_gate_fir : entity work.rx_epoch_gate
        generic map (NUM_STREAMS => 2)
        port map (
            clock => clock, reset => reset,
            in_sample_controls => ctrl, in_samples => f_mux, rx_timestamp => f_mux_ts,
            epoch_arm => arm, epoch_complete => f_mux_cpl, epoch_abort => abort,
            epoch_id_in => epoch_id,
            out_sample_controls => open, out_samples => f_out, out_timestamp => f_ts,
            out_epoch_id => open, out_state => open, out_discard_active => open,
            out_fault => open, epoch_start_event => f_start,
            first_valid_timestamp => f_first
        );

    U_gate_stock : entity work.rx_epoch_gate
        generic map (NUM_STREAMS => 2)
        port map (
            clock => clock, reset => reset,
            in_sample_controls => ctrl, in_samples => s_mux, rx_timestamp => ts,
            epoch_arm => arm, epoch_complete => complete, epoch_abort => abort,
            epoch_id_in => epoch_id,
            out_sample_controls => open, out_samples => s_out, out_timestamp => s_ts,
            out_epoch_id => open, out_state => open, out_discard_active => open,
            out_fault => open, epoch_start_event => s_start,
            first_valid_timestamp => s_first
        );

    main : process
        variable lfsr   : unsigned(31 downto 0) := x"1D872B41";
        variable cyc, idx : integer;
        variable last_idx : integer;
        variable x      : int_arr_t(0 to 4 * NMAX - 1);
        variable ts_in  : ts_arr_t(0 to NMAX - 1);
        variable ts_v   : unsigned(63 downto 0);
        variable fseq, sseq : int_arr_t(0 to 4 * NMAX - 1);
        variable ftsq, stsq : ts_arr_t(0 to NMAX - 1);
        variable nf, ns : integer;
        variable errors : natural := 0;
        variable m1     : integer;
        variable f_opened, s_opened : integer;
        variable fence_cyc : integer;
        variable checks : natural := 0;

        impure function rnd16 return integer is
        begin
            lfsr := lfsr(30 downto 0) & (lfsr(31) xor lfsr(21) xor lfsr(1) xor lfsr(0));
            lfsr := lfsr(30 downto 0) & (lfsr(31) xor lfsr(21) xor lfsr(1) xor lfsr(0));
            return to_integer(signed(lfsr(15 downto 0)));
        end function;

        procedure fail(msg : string) is
        begin
            report msg severity error;
            errors := errors + 1;
        end procedure;

        procedure restart is
        begin
            reset <= '1';
            arm <= '0'; complete <= '0'; abort <= '0';
            adc <= (others => ZERO_SAMPLE);
            for c in 0 to 3 loop
                wait until rising_edge(clock);
            end loop;
            reset <= '0';
            cyc := 0; idx := 0; last_idx := -1; nf := 0; ns := 0;
            ts_v := x"00000002_FFFFFFF0";
            f_opened := -1; s_opened := -1;
        end procedure;

        -- One rx_clock cycle: drive inputs, take the edge, monitor outputs.
        procedure step is
        begin
            if cyc mod 2 = 0 then
                ts_v := ts_v + 1;
            end if;
            ts <= ts_v;
            if last_idx >= 0 then
                ts_in(last_idx) := ts_v;
                last_idx := -1;
            end if;
            if cyc mod 2 = 1 and idx < NMAX then
                for i in 0 to 1 loop
                    x(4 * idx + 2 * i)     := rnd16;
                    x(4 * idx + 2 * i + 1) := rnd16;
                    adc(i).data_i <= to_signed(x(4 * idx + 2 * i), 16);
                    adc(i).data_q <= to_signed(x(4 * idx + 2 * i + 1), 16);
                    adc(i).data_v <= '1';
                end loop;
                last_idx := idx;
                idx := idx + 1;
            else
                adc(0).data_v <= '0';
                adc(1).data_v <= '0';
            end if;
            wait until rising_edge(clock);
            arm <= '0'; complete <= '0'; abort <= '0';
            if f_out(0).data_v = '1' and nf < NMAX then
                fseq(4 * nf to 4 * nf + 3) := (to_integer(f_out(0).data_i),
                    to_integer(f_out(0).data_q), to_integer(f_out(1).data_i),
                    to_integer(f_out(1).data_q));
                ftsq(nf) := f_ts;
                nf := nf + 1;
            end if;
            if s_out(0).data_v = '1' and ns < NMAX then
                sseq(4 * ns to 4 * ns + 3) := (to_integer(s_out(0).data_i),
                    to_integer(s_out(0).data_q), to_integer(s_out(1).data_i),
                    to_integer(s_out(1).data_q));
                stsq(ns) := s_ts;
                ns := ns + 1;
            end if;
            if f_start = '1' then
                if fence_cyc >= 0 and cyc > fence_cyc then
                    fail("T7: FIR gate opened after a cancelling ARM/ABORT, cycle " &
                         integer'image(cyc));
                end if;
                f_opened := cyc;
                if m1 < 0 or f_out(0).data_i /= to_signed(x(4 * (m1 + 2)), 16) or
                   f_out(1).data_q /= to_signed(x(4 * (m1 + 2) + 3), 16) or
                   f_first /= ts_in(m1 + 2) or f_ts /= ts_in(m1 + 2) then
                    fail("T7: FIR gate first sample/timestamp is not x[m1+2]");
                end if;
                checks := checks + 1;
            end if;
            if s_start = '1' then
                s_opened := cyc;
                if m1 < 0 or s_out(0).data_i /= to_signed(x(4 * m1), 16) or
                   s_first /= ts_in(m1) then
                    fail("T7: stock gate first sample/timestamp is not x[m1]");
                end if;
            end if;
            cyc := cyc + 1;
        end procedure;

        procedure run(n : natural) is
        begin
            for c in 1 to n loop
                step;
            end loop;
        end procedure;

        -- Pulse COMPLETE in the next cycle; m1 = first input strobed at/after it.
        procedure do_complete is
        begin
            complete <= '1';
            m1 := idx;  -- idx advances only on a strobe
            step;
        end procedure;

        procedure do_arm(id : natural) is
        begin
            arm <= '1';
            epoch_id <= to_unsigned(id, 8);
            step;  -- a start visible in this cycle was registered before ARM
            m1 := -1;
        end procedure;
    begin
        -- T2 ---------------------------------------------------------------
        m1 := -1; fence_cyc := -1;
        restart;
        run(2 * 2000);
        if nf < 1900 or ns < 1900 then
            fail("T2: too few samples " & integer'image(nf) & " " & integer'image(ns));
        end if;
        for k in 0 to nf - 3 loop
            if fseq(4 * (k + 2) to 4 * (k + 2) + 3) /= sseq(4 * k to 4 * k + 3) or
               ftsq(k + 2) /= stsq(k) then
                fail("T2: FIR chain differs from stock at sample " & integer'image(k));
                exit;
            end if;
        end loop;
        report "T2: " & integer'image(nf - 2) & " samples identical to stock";

        -- T7 open: COMPLETE on a strobe cycle and between strobes ------------
        for phase in 0 to 1 loop
            restart;
            fence_cyc := -1;
            run(41 + phase);
            do_arm(1);
            run(20);
            do_complete;
            run(60);
            if f_opened < 0 or s_opened < 0 then
                fail("T7: gate did not open, phase " & integer'image(phase));
            end if;
        end loop;

        -- T7 cancel: ARM or ABORT d cycles after COMPLETE --------------------
        for kind in 0 to 1 loop
            for d in 1 to 30 loop
                restart;
                fence_cyc := -1;
                run(41 + d mod 2);
                do_arm(3);
                run(20);
                do_complete;
                run(d - 1);
                fence_cyc := cyc;
                if kind = 0 then
                    do_arm(4);
                else
                    abort <= '1';
                    step;
                    m1 := -1;
                end if;
                run(80);
                -- Recovery: a fresh transaction opens normally.
                fence_cyc := -1;
                f_opened := -1;
                do_arm(5);
                run(10);
                do_complete;
                run(60);
                if f_opened < 0 then
                    fail("T7: no recovery after cancel kind " & integer'image(kind) &
                         " d " & integer'image(d));
                end if;
            end loop;
        end loop;

        assert errors = 0
            report "rx_spatial_fir_gate_equiv_tb FAILED: " & integer'image(errors) &
                   " errors" severity failure;
        report "rx_spatial_fir_gate_equiv_tb PASS (T2, T7; " &
               integer'image(checks) & " FIR epoch opens checked)";
        done <= true;
        wait;
    end process;

end architecture;
