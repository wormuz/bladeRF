-- Timestamp-aligned TX baseband fence for an RFPLL transition.
-- The sample/metadata FIFOs continue draining on their original timeline;
-- only the DAC IQ values are replaced with zero from the requested boundary
-- until the host confirms the final RFIC state. This preserves packet/FIFO
-- alignment and makes the muted interval observable instead of replaying
-- stale queued IQ after the retune.

library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library work;
    use work.fifo_readwrite_p.all;

entity tx_retune_gate is
    generic (
        NUM_STREAMS : natural := 2
    );
    port (
        clock               : in  std_logic;
        reset               : in  std_logic;
        timestamp           : in  unsigned(63 downto 0);

        arm                 : in  std_logic := '0';
        arm_immediate       : in  std_logic := '0';
        target_timestamp    : in  unsigned(63 downto 0) := (others => '0');
        complete            : in  std_logic := '0';
        abort               : in  std_logic := '0';

        in_samples          : in  sample_streams_t(0 to NUM_STREAMS-1);
        out_samples         : out sample_streams_t(0 to NUM_STREAMS-1);

        state               : out unsigned(1 downto 0) := "00";
        muted               : out std_logic := '0';
        mute_start_event    : out std_logic := '0';
        resume_event        : out std_logic := '0';
        mute_start_timestamp : out unsigned(63 downto 0) := (others => '0');
        muted_sample_count  : out unsigned(63 downto 0) := (others => '0')
    );
end entity;

architecture rtl of tx_retune_gate is
    constant STATE_ACTIVE : unsigned(1 downto 0) := "00";
    constant STATE_ARMED  : unsigned(1 downto 0) := "01";
    constant STATE_MUTED  : unsigned(1 downto 0) := "10";
    constant STATE_ERROR  : unsigned(1 downto 0) := "11";

    signal state_i : unsigned(1 downto 0) := STATE_ACTIVE;
    signal target_i : unsigned(63 downto 0) := (others => '0');
    signal immediate_i : std_logic := '0';
    signal muted_i : std_logic := '0';
    signal mute_start_timestamp_i : unsigned(63 downto 0) := (others => '0');
    signal muted_sample_count_i : unsigned(63 downto 0) := (others => '0');
begin
    -- Timestamp subtraction is wrap-safe for targets less than 2^63 ticks
    -- away, matching the NIOS scheduled-retune future-time constraint.
    due : process(all)
    begin
        if abort = '1' or
           (arm = '1' and (arm_immediate = '1' or
                           signed(timestamp - target_timestamp) >= 0)) or
           state_i = STATE_MUTED or state_i = STATE_ERROR or
           (state_i = STATE_ARMED and
            (immediate_i = '1' or signed(timestamp - target_i) >= 0)) then
            muted_i <= '1';
        else
            muted_i <= '0';
        end if;
    end process;

    count_suppressed_samples : process(clock, reset)
        variable any_valid : std_logic;
    begin
        if reset = '1' then
            muted_sample_count_i <= (others => '0');
        elsif rising_edge(clock) then
            any_valid := '0';
            for i in in_samples'range loop
                any_valid := any_valid or in_samples(i).data_v;
            end loop;
            if muted_i = '1' and any_valid = '1' then
                muted_sample_count_i <= muted_sample_count_i + 1;
            end if;
        end if;
    end process;

    gate_samples : process(all)
        variable samples_v : sample_streams_t(0 to NUM_STREAMS-1);
    begin
        samples_v := in_samples;
        if muted_i = '1' then
            for i in in_samples'range loop
                samples_v(i).data_i := (others => '0');
                samples_v(i).data_q := (others => '0');
                -- Explicitly keep DAC data-valid asserted so the RFFE path
                -- loads zero even while the host FIFO is momentarily empty.
                -- fifo_reader itself continues consuming timestamped samples.
                samples_v(i).data_v := '1';
            end loop;
        end if;
        out_samples <= samples_v;
    end process;

    state_machine : process(clock, reset)
        variable due_now : boolean;
    begin
        if reset = '1' then
            state_i <= STATE_ACTIVE;
            target_i <= (others => '0');
            immediate_i <= '0';
            mute_start_timestamp_i <= (others => '0');
            mute_start_event <= '0';
            resume_event <= '0';
        elsif rising_edge(clock) then
            mute_start_event <= '0';
            resume_event <= '0';
            due_now := signed(timestamp - target_i) >= 0;

            if abort = '1' then
                state_i <= STATE_ERROR;
                if state_i /= STATE_MUTED and state_i /= STATE_ERROR then
                    mute_start_timestamp_i <= timestamp;
                    mute_start_event <= '1';
                end if;
            elsif arm = '1' then
                if state_i = STATE_ACTIVE then
                    target_i <= target_timestamp;
                    immediate_i <= arm_immediate;
                    if arm_immediate = '1' or
                       signed(timestamp - target_timestamp) >= 0 then
                        state_i <= STATE_MUTED;
                        mute_start_timestamp_i <= timestamp;
                        mute_start_event <= '1';
                    else
                        state_i <= STATE_ARMED;
                    end if;
                elsif state_i = STATE_ERROR then
                    -- Recovery requires a fresh ARM and later COMPLETE;
                    -- ABORT cannot itself reopen TX.
                    target_i <= target_timestamp;
                    immediate_i <= arm_immediate;
                    if arm_immediate = '1' or
                       signed(timestamp - target_timestamp) >= 0 then
                        state_i <= STATE_MUTED;
                        mute_start_timestamp_i <= timestamp;
                        mute_start_event <= '1';
                    else
                        state_i <= STATE_ARMED;
                    end if;
                elsif state_i = STATE_ARMED then
                    -- Requests are ordered by NIOS; retain the earliest
                    -- boundary so queued transitions cannot reopen a gap.
                    if signed(target_i - target_timestamp) > 0 then
                        target_i <= target_timestamp;
                    end if;
                    if arm_immediate = '1' or due_now then
                        state_i <= STATE_MUTED;
                        mute_start_timestamp_i <= timestamp;
                        mute_start_event <= '1';
                    end if;
                end if;
            elsif state_i = STATE_ARMED and
                  (immediate_i = '1' or due_now) then
                state_i <= STATE_MUTED;
                mute_start_timestamp_i <= timestamp;
                mute_start_event <= '1';
            elsif state_i = STATE_MUTED and complete = '1' then
                state_i <= STATE_ACTIVE;
                immediate_i <= '0';
                resume_event <= '1';
            end if;
        end if;
    end process;

    state <= state_i;
    muted <= muted_i;
    mute_start_timestamp <= mute_start_timestamp_i;
    muted_sample_count <= muted_sample_count_i;
end architecture;
