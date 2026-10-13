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
        underflow           : in  std_logic := '0';

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
    signal target_bank0_i : unsigned(63 downto 0) := (others => '0');
    signal target_bank1_i : unsigned(63 downto 0) := (others => '0');
    signal target_bank_select_i : std_logic := '0';
    signal target_bank_active_write_i : std_logic;
    signal target_bank_armed_write_i : std_logic;
    signal target_bank0_write_en_i : std_logic;
    signal target_bank1_write_en_i : std_logic;
    attribute keep : boolean;
    attribute keep of target_bank_active_write_i : signal is TRUE;
    attribute keep of target_bank_armed_write_i : signal is TRUE;
    attribute keep of target_bank0_write_en_i : signal is TRUE;
    attribute keep of target_bank1_write_en_i : signal is TRUE;
    signal target_reached_bank0_i : std_logic;
    signal target_reached_bank1_i : std_logic;
    signal target_reached_i : std_logic;
    signal target_after_bank0_i : std_logic;
    signal target_after_bank1_i : std_logic;
    signal immediate_i : std_logic := '0';
    signal muted_i : std_logic := '0';
    signal abort_i : std_logic;
    signal mute_start_timestamp_i : unsigned(63 downto 0) := (others => '0');
    signal muted_sample_count_i : unsigned(63 downto 0) := (others => '0');

    -- Modulo half-range ordering: a signed 64-bit difference is nonnegative
    -- exactly when current_ts has reached target_ts. Keep this as one native
    -- arithmetic operation so Quartus can map the compare onto the Cyclone V
    -- carry chain; the hand-built 16-bit borrow tree mapped through MLAB/LAB
    -- stages and became the critical timestamp-to-DAC path.
    function timestamp_reached(
        current_ts : unsigned(63 downto 0);
        target_ts  : unsigned(63 downto 0)
    ) return boolean is
        variable delta : signed(63 downto 0);
    begin
        delta := signed(current_ts) - signed(target_ts);
        return delta(63) = '0';
    end function;

    -- Strict modulo ordering for two scheduled timestamps. The caller's
    -- contract limits their separation to less than 2^63 ticks. This avoids
    -- forming a 64-bit difference merely to decide which queued mute
    -- boundary is earlier.
    function timestamp_after(
        left_ts  : unsigned(63 downto 0);
        right_ts : unsigned(63 downto 0)
    ) return boolean is
    begin
        -- For timestamps less than 2^63 apart, timestamps in the same
        -- half-range use ordinary low-63-bit ordering. Across the sign
        -- boundary, the order reverses. Compare the full low field in one
        -- native operator rather than cascading high-word and low-word
        -- comparators; post-fit STA measured that cascade on
        -- target_timestamp -> target_bank_select_i.
        if left_ts(63) /= right_ts(63) then
            return left_ts(62 downto 0) < right_ts(62 downto 0);
        end if;
        return left_ts(62 downto 0) > right_ts(62 downto 0);
    end function;
begin
    -- Compare each target bank in parallel and select the one-bit result.
    -- This keeps the bank selector out of the 64-bit timestamp comparator.
    target_reached_bank0_i <= '1' when timestamp_reached(timestamp, target_bank0_i)
                              else '0';
    target_reached_bank1_i <= '1' when timestamp_reached(timestamp, target_bank1_i)
                              else '0';
    target_reached_i <= target_reached_bank1_i when target_bank_select_i = '1'
                        else target_reached_bank0_i;
    target_after_bank0_i <= '1' when timestamp_after(target_bank0_i, target_timestamp)
                            else '0';
    target_after_bank1_i <= '1' when timestamp_after(target_bank1_i, target_timestamp)
                            else '0';

    -- FIFO underflow is a local data-plane fault. Mute in this gate rather
    -- than waiting for the host's periodic fault monitor.
    abort_i <= abort or underflow;

    -- Factor command acceptance from bank selection. The fitted path was
    -- selector-Q into the 64-bit bank D mux; with this structure it drives
    -- only the local bank-enable gates.
    target_bank_active_write_i <= '1' when abort_i = '0' and arm = '1' and
        (state_i = STATE_ACTIVE or state_i = STATE_ERROR) else '0';
    target_bank_armed_write_i <= '1' when abort_i = '0' and arm = '1' and
        state_i = STATE_ARMED else '0';
    target_bank0_write_en_i <= target_bank_active_write_i or
        (target_bank_armed_write_i and target_bank_select_i);
    target_bank1_write_en_i <= target_bank_armed_write_i and
        not target_bank_select_i;

    -- Timestamp subtraction is wrap-safe for targets less than 2^63 ticks
    -- away, matching the NIOS scheduled-retune future-time constraint.
    due : process(all)
    begin
        if abort_i = '1' or
           (arm = '1' and (arm_immediate = '1' or
                           timestamp_reached(timestamp, target_timestamp))) or
           state_i = STATE_MUTED or state_i = STATE_ERROR or
           (state_i = STATE_ARMED and
            (immediate_i = '1' or target_reached_i = '1')) then
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

    -- Keep target storage writes independent of the scheduled-boundary
    -- comparison.  In STATE_ARMED the comparison can toggle the active-bank
    -- selector, while the incoming request is written to the bank that was
    -- inactive at the start of this cycle.  Combining these in the FSM made
    -- the comparator/selector feedback path also drive the 64-bit bank D
    -- inputs in the fitted netlist.
    target_banks : process(clock, reset)
    begin
        if reset = '1' then
            target_bank0_i <= (others => '0');
            target_bank1_i <= (others => '0');
        elsif rising_edge(clock) then
            if target_bank0_write_en_i = '1' then
                target_bank0_i <= target_timestamp;
            end if;
            if target_bank1_write_en_i = '1' then
                target_bank1_i <= target_timestamp;
            end if;
        end if;
    end process;

    state_machine : process(clock, reset)
        variable due_now : boolean;
        variable next_bank_select : std_logic;
    begin
        if reset = '1' then
            state_i <= STATE_ACTIVE;
            target_bank_select_i <= '0';
            immediate_i <= '0';
            mute_start_timestamp_i <= (others => '0');
            mute_start_event <= '0';
            resume_event <= '0';
        elsif rising_edge(clock) then
            mute_start_event <= '0';
            resume_event <= '0';
            due_now := target_reached_i = '1';

            if abort_i = '1' then
                state_i <= STATE_ERROR;
                if state_i /= STATE_MUTED and state_i /= STATE_ERROR then
                    mute_start_timestamp_i <= timestamp;
                    mute_start_event <= '1';
                end if;
            elsif arm = '1' then
                if state_i = STATE_ACTIVE then
                    target_bank_select_i <= '0';
                    immediate_i <= arm_immediate;
                    if arm_immediate = '1' or
                       timestamp_reached(timestamp, target_timestamp) then
                        state_i <= STATE_MUTED;
                        mute_start_timestamp_i <= timestamp;
                        mute_start_event <= '1';
                    else
                        state_i <= STATE_ARMED;
                    end if;
                elsif state_i = STATE_ERROR then
                    -- Recovery after an abort remains muted immediately.
                    -- Waiting until a future scheduled boundary here would
                    -- briefly replay stale IQ while RFIC state is unknown.
                    -- A fresh ARM and later COMPLETE are both required to
                    -- resume the stream.
                    target_bank_select_i <= '0';
                    immediate_i <= arm_immediate;
                    state_i <= STATE_MUTED;
                elsif state_i = STATE_ARMED then
                    -- Requests are ordered by NIOS; retain the earliest
                    -- boundary so queued transitions cannot reopen a gap.
                    -- Keep the earliest requested boundary while a retune
                    -- is already armed. Write data to the inactive bank so
                    -- this comparator controls only the bank selector.
                    -- Keep the measured selector structure from the positive
                    -- slack hosted fit: choose the active-bank comparison,
                    -- then toggle only when the active boundary is later.
                    -- Feed each parallel comparison directly into the
                    -- selector next-state equation. This removes the
                    -- comparator-result mux followed by conditional-toggle
                    -- logic on the measured target_timestamp-to-selector
                    -- critical path.
                    next_bank_select :=
                        (target_bank_select_i and not target_after_bank1_i) or
                        (not target_bank_select_i and target_after_bank0_i);
                    target_bank_select_i <= next_bank_select;
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
