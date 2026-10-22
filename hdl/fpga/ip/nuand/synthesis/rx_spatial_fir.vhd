-- 2x2 complex MIMO FIR (5 taps) for RX spatial whitening, rx_clock domain.
-- Design: doc/development/rx-spatial-mimo-fir-design.md, section 1 (datapath)
-- and section 3 (delayed epoch COMPLETE). Golden bit-true model:
-- sdr-scanner rfcore/spatial_fir.py (apply_fixed).
--     y_i[n] = sum_j sum_k h_ij[k] * x_j[n-k],  k = 0..4, center tap 2
-- Data SC16, coefficients Q2.14 (1.0 = 16384), products summed at full width,
-- round (+2^13, asr 14), saturate to [-2048, 2047]. Output sample n carries
-- the timestamp of its center input x[n-2]. Constant latency 9 rx_clock cycles
-- from input data_v to output data_v in every mode.
-- Bypass (pure pass-through of x_own[n-2], no rounding or saturation) when any
-- lane is disabled (RX_X1), the active bank is the identity, or force_identity
-- is set. The mode bit and the coefficient bank are sampled once per sample at
-- S1, so a commit lands on an output-sample boundary.
-- Coefficient banks live here: shadow (written word by word) and active
-- (replaced by commit in one rx_clock edge). The controller in front of this
-- block (CDC + toggle decode) drives coef_wr_* / coef_commit as rx_clock
-- pulses. A commit in the same cycle as a write takes the shadow before that
-- write.
--
-- Copyright (c) 2026 bladeRF project contributors
--
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
--
-- The above copyright notice and this permission notice shall be included in
-- all copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
-- THE SOFTWARE.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity rx_spatial_fir is
    port (
        clock               : in  std_logic;
        reset               : in  std_logic;

        in_sample_controls  : in  sample_controls_t(0 to 1);
        in_samples          : in  sample_streams_t(0 to 1);
        rx_timestamp        : in  unsigned(63 downto 0);

        -- Single-cycle rx_clock pulses, same as rx_epoch_gate inputs.
        epoch_arm           : in  std_logic := '0';
        epoch_complete      : in  std_logic := '0';
        epoch_abort         : in  std_logic := '0';

        -- Bank control (rx_clock). Word: [15:0] real, [31:16] imag, Q2.14.
        -- Index (i*2 + j)*5 + k: output lane i, input lane j, tap k.
        coef_wr_en          : in  std_logic := '0';
        coef_wr_idx         : in  unsigned(4 downto 0) := (others => '0');
        coef_wr_data        : in  std_logic_vector(31 downto 0) := (others => '0');
        coef_commit         : in  std_logic := '0';
        coef_commit_tag     : in  unsigned(7 downto 0) := (others => '0');
        force_identity      : in  std_logic := '0';

        out_samples         : out sample_streams_t(0 to 1);
        out_timestamp       : out unsigned(63 downto 0);
        -- Delayed COMPLETE for rx_epoch_gate (via the rx_mux register).
        -- Combinationally masked by ARM/ABORT so a COMPLETE leaving the block
        -- in the same cycle as a newer ARM never reaches the gate.
        out_epoch_complete  : out std_logic;
        -- First output sample whose 5-sample support is newer than COMPLETE.
        out_support_clean   : out std_logic;

        -- Status (rx_clock levels).
        out_active_tag      : out unsigned(7 downto 0);
        out_bypass          : out std_logic;
        out_active_identity : out std_logic;
        out_both_lanes      : out std_logic;
        out_sat_sticky      : out std_logic_vector(1 downto 0)
    );
end entity;

architecture rtl of rx_spatial_fir is

    constant TAPS     : natural := 5;
    constant CENTER   : natural := 2;
    constant NCOEF    : natural := 20;
    constant PROD_W   : natural := 34;  -- hr*xr + (-hi)*xi: |.| <= 2^31
    constant ACC_W    : natural := 38;  -- 10 sum-of-two results, design 1.2
    constant ONE_Q14  : signed(15 downto 0) := to_signed(16384, 16);
    constant SAT_HI   : integer := 2047;
    constant SAT_LO   : integer := -2048;
    constant STAGES   : natural := 9;   -- S0..S8

    type tap_line_t   is array (0 to TAPS-1) of signed(15 downto 0);
    type lane_line_t  is array (0 to 1) of tap_line_t;
    type coef_word_t  is array (0 to NCOEF-1) of std_logic_vector(31 downto 0);
    type c16_t        is array (0 to NCOEF-1) of signed(15 downto 0);
    type c17_t        is array (0 to NCOEF-1) of signed(16 downto 0);
    type prod_t       is array (0 to NCOEF-1) of signed(PROD_W-1 downto 0);
    type acc5_t       is array (0 to 4) of signed(ACC_W-1 downto 0);
    type acc3_t       is array (0 to 2) of signed(ACC_W-1 downto 0);
    type acc2_t       is array (0 to 1) of signed(ACC_W-1 downto 0);
    type lvl1_t       is array (0 to 1) of acc5_t;
    type lvl2_t       is array (0 to 1) of acc3_t;
    type lvl3_t       is array (0 to 1) of acc2_t;
    type y12_t        is array (0 to 1) of signed(15 downto 0);
    type ts_line_t    is array (0 to CENTER) of unsigned(63 downto 0);
    type ts_pipe_t    is array (2 to STAGES-2) of unsigned(63 downto 0);
    type byp_pipe_t   is array (1 to STAGES-2) of sample_streams_t(0 to 1);
    type lv_pipe_t    is array (1 to STAGES-2) of std_logic_vector(1 downto 0);

    function idx_of(i, j, k : natural) return natural is
    begin
        return (i * 2 + j) * TAPS + k;
    end function;

    function identity_word(idx : natural) return std_logic_vector is
        variable rv : std_logic_vector(31 downto 0) := (others => '0');
    begin
        if idx = idx_of(0, 0, CENTER) or idx = idx_of(1, 1, CENTER) then
            rv(15 downto 0) := std_logic_vector(ONE_Q14);
        end if;
        return rv;
    end function;

    function identity_bank return coef_word_t is
        variable rv : coef_word_t;
    begin
        for n in rv'range loop
            rv(n) := identity_word(n);
        end loop;
        return rv;
    end function;

    -- Round (+2^13, asr 14), saturate to 12 bits. Result: saturated & value.
    function round_sat(a : signed(ACC_W-1 downto 0)) return signed is
        variable r : signed(ACC_W-1 downto 0);
    begin
        r := shift_right(a + 2**13, 14);
        if r > SAT_HI then
            return '1' & to_signed(SAT_HI, 16);
        elsif r < SAT_LO then
            return '1' & to_signed(SAT_LO, 16);
        end if;
        return '0' & resize(r, 16);
    end function;

    -- Banks
    signal shadow          : coef_word_t := identity_bank;
    signal shadow_word_id  : std_logic_vector(NCOEF-1 downto 0) := (others => '1');
    signal shadow_all_id   : std_logic;
    signal act_identity    : std_logic := '1';
    signal act_tag         : unsigned(7 downto 0) := (others => '0');
    signal bank_gen        : std_logic := '0';

    -- S0: delay lines, timestamp line, sideband
    signal dl_i, dl_q      : lane_line_t := (others => (others => (others => '0')));
    signal ts_line         : ts_line_t := (others => (others => '0'));
    signal ep_pending      : std_logic := '0';
    signal ep_count        : unsigned(2 downto 0) := (others => '0');

    -- Sideband pipeline. Stage 0 separate (driven by `front`); v(s): sample
    -- valid at stage s.
    signal v0, mark0, clean0 : std_logic := '0';
    signal lv0             : std_logic_vector(1 downto 0) := (others => '0');
    signal v               : std_logic_vector(1 to STAGES-2) := (others => '0');
    signal lv              : lv_pipe_t := (others => (others => '0'));
    signal mark            : std_logic_vector(1 to STAGES-2) := (others => '0');
    signal clean           : std_logic_vector(1 to STAGES-2) := (others => '0');
    signal mode            : std_logic_vector(1 to STAGES-2) := (others => '1');
    signal gen             : std_logic_vector(1 to STAGES-2) := (others => '0');
    signal byp             : byp_pipe_t := (others => (others => ZERO_SAMPLE));
    signal ts_p            : ts_pipe_t := (others => (others => '0'));

    -- Arithmetic (no reset: DSP input/output registers)
    signal s1_xi, s1_xq    : lane_line_t;
    signal s1_re, s1_im    : c16_t;
    signal s1_nim          : c17_t;
    signal s2_pr, s2_pi    : prod_t;
    signal s3_r, s3_i      : lvl1_t;
    signal s4_r, s4_i      : lvl2_t;
    signal s5_r, s5_i      : lvl3_t;
    signal s6_r, s6_i      : acc2_t;  -- per lane
    signal s7_r, s7_i      : y12_t;
    signal s7_sat          : std_logic_vector(1 downto 0);
    signal both_lanes_q    : std_logic := '0';
    signal sat_sticky      : std_logic_vector(1 downto 0) := (others => '0');
    signal complete_q      : std_logic := '0';
begin

    -- The active coefficients live in the S1 multiplier input registers (see
    -- arith); this process keeps only the shadow bank and the active bank's
    -- identity flag, tag and generation.
    shadow_all_id <= '1' when shadow_word_id = (shadow_word_id'range => '1') else '0';

    banks : process(clock, reset)
        variable n : natural;
    begin
        if reset = '1' then
            shadow         <= identity_bank;
            shadow_word_id <= (others => '1');
            act_identity   <= '1';
            act_tag        <= (others => '0');
            bank_gen       <= '0';
        elsif rising_edge(clock) then
            -- The shadow -> S1 hold waiver (bladerf.sdc) rests on this.
            assert not (coef_wr_en = '1' and coef_commit = '1')
                report "rx_spatial_fir: shadow write and commit in one cycle"
                severity failure;
            if coef_commit = '1' then
                act_identity <= shadow_all_id;
                act_tag      <= coef_commit_tag;
                bank_gen     <= not bank_gen;
            end if;
            if coef_wr_en = '1' and coef_wr_idx < NCOEF then
                n := to_integer(coef_wr_idx);
                shadow(n) <= coef_wr_data;
                if coef_wr_data = identity_word(n) then
                    shadow_word_id(n) <= '1';
                else
                    shadow_word_id(n) <= '0';
                end if;
            end if;
        end if;
    end process;

    -- S0 delay lines and epoch COMPLETE counter; S1 timestamp line.
    front : process(clock, reset)
        variable strobe : std_logic;
        variable cnt    : unsigned(2 downto 0);
        variable act    : std_logic;
        variable m4, m5 : std_logic;
    begin
        if reset = '1' then
            dl_i       <= (others => (others => (others => '0')));
            dl_q       <= (others => (others => (others => '0')));
            ts_line    <= (others => (others => '0'));
            ep_pending <= '0';
            ep_count   <= (others => '0');
            v0         <= '0';
            lv0        <= (others => '0');
            mark0      <= '0';
            clean0     <= '0';
        elsif rising_edge(clock) then
            -- One common shift strobe: in RX_X2 both lanes strobe together; in
            -- RX_X1 the enabled lane alone must still advance the lines.
            strobe := in_samples(0).data_v or in_samples(1).data_v;
            if strobe = '1' then
                for j in 0 to 1 loop
                    dl_i(j)(0) <= in_samples(j).data_i;
                    dl_q(j)(0) <= in_samples(j).data_q;
                    for k in 1 to TAPS-1 loop
                        dl_i(j)(k) <= dl_i(j)(k-1);
                        dl_q(j)(k) <= dl_q(j)(k-1);
                    end loop;
                end loop;
            end if;
            v0    <= strobe;
            lv0   <= in_samples(1).data_v & in_samples(0).data_v;

            -- ts_in(m) = rx_timestamp one cycle after x[m]'s data_v, which is
            -- what the stock gate samples behind the one-register rx_mux.
            if v0 = '1' then
                ts_line(0) <= rx_timestamp;
                for k in 1 to CENTER loop
                    ts_line(k) <= ts_line(k-1);
                end loop;
            end if;

            -- A strobe coincident with COMPLETE counts as the first sample
            -- after it, as in the stock chain (the gate admits that sample).
            m4 := '0'; m5 := '0';
            if epoch_arm = '1' or epoch_abort = '1' then
                ep_pending <= '0';
                ep_count   <= (others => '0');
            else
                act := ep_pending or epoch_complete;
                cnt := ep_count;
                if epoch_complete = '1' then
                    cnt := (others => '0');
                end if;
                if act = '1' and strobe = '1' then
                    cnt := cnt + 1;
                    if cnt = TAPS - 1 then
                        m4 := '1';
                    end if;
                    if cnt = TAPS then
                        m5 := '1';
                    end if;
                end if;
                ep_count <= cnt;
                if m5 = '1' then
                    ep_pending <= '0';
                else
                    ep_pending <= act;
                end if;
            end if;
            mark0    <= m4;
            clean0   <= m5;
        end if;
    end process;

    -- Sideband carry S1..S7 (valid, lane valid, epoch marks, mode, bank
    -- generation, bypass data, timestamp).
    sideband : process(clock, reset)
    begin
        if reset = '1' then
            v                    <= (others => '0');
            lv                   <= (others => (others => '0'));
            mark                 <= (others => '0');
            clean                <= (others => '0');
            mode                 <= (others => '1');
            gen                  <= (others => '0');
            both_lanes_q         <= '0';
        elsif rising_edge(clock) then
            v(1)     <= v0;
            lv(1)    <= lv0;
            mark(1)  <= mark0;
            clean(1) <= clean0;
            for s in 2 to STAGES-2 loop
                v(s)     <= v(s-1);
                lv(s)    <= lv(s-1);
                mark(s)  <= mark(s-1);
                clean(s) <= clean(s-1);
                mode(s)  <= mode(s-1);
                gen(s)   <= gen(s-1);
            end loop;
            -- S1: mode and bank generation sampled with the coefficients.
            -- The bank after this edge: S1 takes a commit on this edge.
            if in_sample_controls(0).enable = '0' or
               in_sample_controls(1).enable = '0' or
               (coef_commit = '1' and shadow_all_id = '1') or
               (coef_commit = '0' and act_identity = '1') or
               force_identity = '1' then
                mode(1) <= '1';
            else
                mode(1) <= '0';
            end if;
            gen(1) <= bank_gen xor coef_commit;
            both_lanes_q <= in_sample_controls(0).enable and
                            in_sample_controls(1).enable;
            -- A newer ARM/ABORT cancels a COMPLETE already in flight.
            if epoch_arm = '1' or epoch_abort = '1' then
                mark <= (others => '0');
            end if;
        end if;
    end process;

    -- Bypass data and timestamp carry, clock-enabled by the stage valid.
    data_carry : process(clock)
    begin
        if rising_edge(clock) then
            if v0 = '1' then
                for j in 0 to 1 loop
                    byp(1)(j).data_i <= dl_i(j)(CENTER);
                    byp(1)(j).data_q <= dl_q(j)(CENTER);
                end loop;
            end if;
            for s in 2 to STAGES-2 loop
                if v(s-1) = '1' then
                    byp(s) <= byp(s-1);
                end if;
            end loop;
            -- S2 reads the line one edge after the S1 shift of this sample.
            if v(1) = '1' then
                ts_p(2) <= ts_line(CENTER);
            end if;
            for s in 3 to STAGES-2 loop
                if v(s-1) = '1' then
                    ts_p(s) <= ts_p(s-1);
                end if;
            end loop;
        end if;
    end process;

    -- S1..S7 arithmetic. No reset, so Quartus can pack the S1 (input) and S2
    -- (output) registers into the DSP blocks (sum-of-two 18x19 mode, one
    -- block per (i, j, k, Re/Im)); each stage is clock-enabled by the valid of
    -- the sample entering it (DSP ENA port).
    arith : process(clock)
        variable n : natural;
        variable qr, qi : signed(16 downto 0);
    begin
        if rising_edge(clock) then
            -- S1. The coefficient registers are the active bank, loaded from
            -- the shadow on commit (their own DSP ENA). A separate active
            -- bank copied here on every sample was a zero-logic LAB -> DSP
            -- input path that failed hold. The sample loaded on the edge of
            -- a commit uses the new bank; sideband gen/mode follow (S1).
            -- Shadow -> S1 hold is waived in bladerf.sdc: the controller
            -- never asserts coef_wr_en and coef_commit in the same cycle.
            if v0 = '1' then
                s1_xi  <= dl_i;
                s1_xq  <= dl_q;
            end if;
            if coef_commit = '1' then
                for m in 0 to NCOEF-1 loop
                    s1_re(m)  <= signed(shadow(m)(15 downto 0));
                    s1_im(m)  <= signed(shadow(m)(31 downto 16));
                    s1_nim(m) <= -resize(signed(shadow(m)(31 downto 16)), 17);
                end loop;
            end if;
            -- S2: Re = hr*xr + (-hi)*xi, Im = hr*xi + hi*xr
            if v(1) = '1' then
                for i in 0 to 1 loop
                    for j in 0 to 1 loop
                        for k in 0 to TAPS-1 loop
                            n := idx_of(i, j, k);
                            s2_pr(n) <= resize(s1_re(n) * s1_xi(j)(k), PROD_W) +
                                        resize(s1_nim(n) * s1_xq(j)(k), PROD_W);
                            s2_pi(n) <= resize(s1_re(n) * s1_xq(j)(k), PROD_W) +
                                        resize(s1_im(n) * s1_xi(j)(k), PROD_W);
                        end loop;
                    end loop;
                end loop;
            end if;
            -- S3..S6: adder tree 10 -> 5 -> 3 -> 2 -> 1 per component
            for i in 0 to 1 loop
                if v(2) = '1' then
                    for a in 0 to 4 loop
                        n := i * 2 * TAPS + 2 * a;
                        s3_r(i)(a) <= resize(s2_pr(n), ACC_W) + resize(s2_pr(n+1), ACC_W);
                        s3_i(i)(a) <= resize(s2_pi(n), ACC_W) + resize(s2_pi(n+1), ACC_W);
                    end loop;
                end if;
                if v(3) = '1' then
                    s4_r(i)(0) <= s3_r(i)(0) + s3_r(i)(1);
                    s4_r(i)(1) <= s3_r(i)(2) + s3_r(i)(3);
                    s4_r(i)(2) <= s3_r(i)(4);
                    s4_i(i)(0) <= s3_i(i)(0) + s3_i(i)(1);
                    s4_i(i)(1) <= s3_i(i)(2) + s3_i(i)(3);
                    s4_i(i)(2) <= s3_i(i)(4);
                end if;
                if v(4) = '1' then
                    s5_r(i)(0) <= s4_r(i)(0) + s4_r(i)(1);
                    s5_r(i)(1) <= s4_r(i)(2);
                    s5_i(i)(0) <= s4_i(i)(0) + s4_i(i)(1);
                    s5_i(i)(1) <= s4_i(i)(2);
                end if;
                if v(5) = '1' then
                    s6_r(i) <= s5_r(i)(0) + s5_r(i)(1);
                    s6_i(i) <= s5_i(i)(0) + s5_i(i)(1);
                end if;
                -- S7
                if v(6) = '1' then
                    qr := round_sat(s6_r(i));
                    qi := round_sat(s6_i(i));
                    s7_r(i)   <= qr(15 downto 0);
                    s7_i(i)   <= qi(15 downto 0);
                    s7_sat(i) <= qr(16) or qi(16);
                end if;
            end loop;
        end if;
    end process;

    -- S8: output select, status.
    output : process(clock, reset)
    begin
        if reset = '1' then
            out_samples    <= (others => ZERO_SAMPLE);
            out_timestamp  <= (others => '0');
            complete_q     <= '0';
            out_support_clean <= '0';
            sat_sticky     <= (others => '0');
        elsif rising_edge(clock) then
            for i in 0 to 1 loop
                if mode(STAGES-2) = '1' then
                    out_samples(i).data_i <= byp(STAGES-2)(i).data_i;
                    out_samples(i).data_q <= byp(STAGES-2)(i).data_q;
                else
                    out_samples(i).data_i <= s7_r(i);
                    out_samples(i).data_q <= s7_i(i);
                end if;
                out_samples(i).data_v <= lv(STAGES-2)(i);
            end loop;
            out_timestamp     <= ts_p(STAGES-2);
            complete_q        <= v(STAGES-2) and mark(STAGES-2) and
                                 not (epoch_arm or epoch_abort);
            out_support_clean <= v(STAGES-2) and clean(STAGES-2);
            if coef_commit = '1' then
                sat_sticky <= (others => '0');
            else
                -- Only samples computed with the bank now active count.
                for i in 0 to 1 loop
                    if lv(STAGES-2)(i) = '1' and mode(STAGES-2) = '0' and
                       gen(STAGES-2) = bank_gen and s7_sat(i) = '1' then
                        sat_sticky(i) <= '1';
                    end if;
                end loop;
            end if;
        end if;
    end process;

    out_epoch_complete  <= complete_q and not (epoch_arm or epoch_abort);
    out_active_tag      <= act_tag;
    out_bypass          <= mode(1);
    out_active_identity <= act_identity;
    out_both_lanes      <= both_lanes_q;
    out_sat_sticky      <= sat_sticky;

end architecture;
