-- DAC output stage: the TX retune gate's mute applied after registers.
--
-- The DAC data used to be the gate's combinational output (zero while muted,
-- data_v forced so the zero loads) registered in bladerf_core and held while
-- no sample was valid. Here the raw sample and the mute decision are
-- registered instead and the zero select follows them, so the mute logic
-- (64-bit timestamp compares, FSM) ends at a register instead of running
-- through the select into the DAC path. Physical-synthesis retiming had
-- pulled that path between two registers: setup -0.009 ns, hosted A4
-- 2026-10-22. dac_zero carries a zero output through invalid cycles exactly
-- as the old hold did, so out_samples equals the old register on every edge
-- (tx_dac_output_tb).

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity tx_dac_output is
    generic (
        NUM_STREAMS : natural := 2
    );
    port (
        clock       : in  std_logic;
        in_samples  : in  sample_streams_t(0 to NUM_STREAMS-1);
        muted       : in  std_logic;
        -- data_v is always '1': the DAC takes the held value every cycle.
        out_samples : out sample_streams_t(0 to NUM_STREAMS-1)
    );
end entity;

architecture rtl of tx_dac_output is
    signal hold : sample_streams_t(0 to NUM_STREAMS-1) := (others => ZERO_SAMPLE);
    signal zero : std_logic_vector(0 to NUM_STREAMS-1) := (others => '1');
begin
    registers : process(clock)
    begin
        if rising_edge(clock) then
            for i in in_samples'range loop
                if in_samples(i).data_v = '1' then
                    hold(i) <= in_samples(i);
                end if;
                zero(i) <= muted or (zero(i) and not in_samples(i).data_v);
            end loop;
        end if;
    end process;

    select_zero : process(all)
    begin
        for i in out_samples'range loop
            out_samples(i) <= hold(i);
            out_samples(i).data_v <= '1';
            if zero(i) = '1' then
                out_samples(i).data_i <= (others => '0');
                out_samples(i).data_q <= (others => '0');
            end if;
        end loop;
    end process;
end architecture;
