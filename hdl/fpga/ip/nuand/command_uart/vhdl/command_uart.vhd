-- Copyright (c) 2015 Nuand LLC
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

library ieee ;
    use ieee.std_logic_1164.all ;
    use ieee.numeric_std.all ;

entity command_uart is
  port (
    -- FPGA internal interface to the TX and RX FIFOs
    clock           :   in  std_logic ;
    reset           :   in  std_logic ;

    -- RS232 interface
    rs232_sout      :   out std_logic ;
    rs232_sin       :   in  std_logic ;

    -- FX3 link/session state, supplied in the command UART clock domain.
    session_reset      : in std_logic := '0' ;
    session_generation : in std_logic_vector(31 downto 0) := (others => '0') ;

    -- Avalon-MM interface
    addr            :   in  std_logic_vector(4 downto 0) ;
    dout            :   out std_logic_vector(31 downto 0) ;
    din             :   in  std_logic_vector(31 downto 0) ;
    read            :   in  std_logic ;
    write           :   in  std_logic ;
    waitreq         :   out std_logic ;
    readack         :   out std_logic ;
    irq             :   out std_logic
  ) ;
end entity ; -- command_uart

architecture arch of command_uart is

    -- TODO: Make this a generic instead of a constant
    constant CLOCKS_PER_BIT :   natural := 20 ;

    -- Send and receive state machine definitions
    type send_fsm_t is (IDLE, SEND_START, SEND_DATA, SEND_STOP ) ;
    type receive_fsm_t is (WAIT_FOR_START, CENTER_START, SAMPLE_DATA, WAIT_FOR_STOP) ;

    -- FSM signals
    signal send_fsm     : send_fsm_t ;
    signal receive_fsm  : receive_fsm_t ;

    -- Synchronization signals for asynchronous input
    signal reg_sin      : std_logic ;
    signal sync_sin     : std_logic ;

    signal reg_response : std_logic_vector(127 downto 0) ;
    -- Avalon writes stage a response here. The UART serializes immutable
    -- snapshots so later response writes cannot alter an active frame.
    signal tx_response : std_logic_vector(127 downto 0) := (others => '0');
    signal pending_response : std_logic_vector(127 downto 0) := (others => '0');
    signal pending_response_valid : std_logic := '0';
    signal response_active : std_logic := '0';
    signal response_overflow : std_logic := '0';
    signal reg_request  : std_logic_vector(127 downto 0) ;
    signal request_session  : std_logic_vector(31 downto 0) := (others => '0');
    signal response_session : std_logic_vector(31 downto 0) := (others => '0');
    signal request_busy     : std_logic := '0';

    signal sin_data     : std_logic_vector(7 downto 0) ;
    signal sin_we       : std_logic ;

    signal sout_data    : std_logic_vector(7 downto 0) ;
    signal sout_we      : std_logic ;

    signal rs232_sout_reg : std_logic;

    signal ack : std_logic ;

    type magics_t is array(natural range <>) of std_logic_vector(7 downto 0) ;

    -- These are all the magic header characters
    constant magics : magics_t := (
        std_logic_vector(to_unsigned(character'pos('A'),8)),    -- 8x8
        std_logic_vector(to_unsigned(character'pos('B'),8)),    -- 8x16
        std_logic_vector(to_unsigned(character'pos('C'),8)),    -- 8x32
        std_logic_vector(to_unsigned(character'pos('D'),8)),    -- 8x64
        std_logic_vector(to_unsigned(character'pos('E'),8)),    -- 16x64
        std_logic_vector(to_unsigned(character'pos('K'),8)),    -- 32x32
        std_logic_vector(to_unsigned(character'pos('N'),8)),    -- Legacy
        std_logic_vector(to_unsigned(character'pos('T'),8)),    -- Retune
        std_logic_vector(to_unsigned(character'pos('U'),8))     -- Retune2
    ) ;

    signal command_in : std_logic ;

    signal control  :   std_logic_vector(7 downto 0) := (0 => '1', others =>'0') ;

    alias isr_enable is control(0) ;
    alias worker_busy is control(1) ;

begin

    readack <= ack ;
    waitreq <= not(ack or write) ;

    -- Avalon-MM interface for responses
    mm_writes : process(clock)
    begin
        if( rising_edge(clock) ) then
            command_in <= '0' ;
            if( write = '1' ) then
                case to_integer(unsigned(addr)) is
                    when  0| 1| 2| 3 => reg_response( 31 downto   0) <= din ;
                    when  4| 5| 6| 7 => reg_response( 63 downto  32) <= din ;
                    when  8| 9|10|11 => reg_response( 95 downto  64) <= din ;
                    -- NIOS writes low-to-high. The last word is the atomic
                    -- commit point after both payload and session are set.
                    when 12|13|14|15 =>
                        reg_response(127 downto  96) <= din ;
                        command_in <= '1' ;
                    when 16|17|18|19 => control(7 downto 0) <= din(7 downto 0) ;
                    when 24|25|26|27 => response_session <= din ;
                    when others => null ;
                end case ;
            end if ;
        end if ;
    end process ;

    send_command : process(clock, reset, session_reset)
        type state_t is (WAITING_FOR_COMMAND, SEND_BYTE, WAITING_FOR_START, WAITING_FOR_DONE) ;
        variable state : state_t ;
        variable count : natural range 0 to 15 := 15 ;
        variable commit_consumed : boolean;
        variable pending_slot_available : boolean;
    begin
        if( reset = '1' or session_reset = '1' ) then
            state := WAITING_FOR_COMMAND ;
            sout_we <= '0' ;
            tx_response <= (others => '0');
            pending_response <= (others => '0');
            pending_response_valid <= '0';
            response_active <= '0';
            response_overflow <= '0';
        elsif( rising_edge(clock) ) then
            sout_we <= '0' ;
            commit_consumed := false;
            pending_slot_available := (pending_response_valid = '0');
            sout_data <= tx_response(count*8+7 downto count*8) ;
            case state is
                when WAITING_FOR_COMMAND =>
                    if (pending_response_valid = '1') then
                        tx_response <= pending_response;
                        pending_response_valid <= '0';
                        pending_slot_available := true;
                        response_active <= '1';
                        state := SEND_BYTE;
                        count := 0;
                    elsif( command_in = '1' and response_overflow = '0' and
                        response_session = session_generation ) then
                        tx_response <= reg_response;
                        response_active <= '1';
                        commit_consumed := true;
                        state := SEND_BYTE ;
                        count := 0 ;
                    end if ;

                when SEND_BYTE =>
                    sout_we <= '1' ;
                    state := WAITING_FOR_START ;

                when WAITING_FOR_START =>
                    if( send_fsm /= IDLE ) then
                        state := WAITING_FOR_DONE ;
                    end if ;

                when WAITING_FOR_DONE =>
                    if( send_fsm = IDLE ) then
                        if( count < 15 ) then
                            count := count + 1 ;
                            state := SEND_BYTE ;
                        elsif (pending_response_valid = '1') then
                            tx_response <= pending_response;
                            pending_response_valid <= '0';
                            pending_slot_available := true;
                            response_active <= '1';
                            count := 0;
                            state := SEND_BYTE;
                        elsif (command_in = '1' and response_overflow = '0' and
                               response_session = session_generation) then
                            -- A commit on the final stop-bit boundary is
                            -- consumed directly rather than lost.
                            tx_response <= reg_response;
                            response_active <= '1';
                            commit_consumed := true;
                            count := 0;
                            state := SEND_BYTE;
                        else
                            response_active <= '0';
                            state := WAITING_FOR_COMMAND ;
                        end if ;
                    end if ;
            end case ;

            -- One committed response may wait behind the active frame. The
            -- NIOS writer checks bit 2 (pending slot full) before staging a
            -- new response; overflow is still surfaced as a sticky fault.
            if (command_in = '1' and
                response_overflow = '0' and
                response_session = session_generation and
                not commit_consumed and state /= WAITING_FOR_COMMAND) then
                if (pending_slot_available) then
                    pending_response <= reg_response;
                    pending_response_valid <= '1';
                else
                    response_overflow <= '1';
                end if;
            end if;
        end if ;
    end process ;

    -- Avanlon-MM interface for requests
    mm_reads : process(clock)
    begin
        if( rising_edge(clock) ) then
            ack <= read ;
            if( read = '1' ) then
                case to_integer(unsigned(addr)) is
                    when  0| 1| 2| 3 => dout <= reg_request( 31 downto  0) ;
                    when  4| 5| 6| 7 => dout <= reg_request( 63 downto 32) ;
                    when  8| 9|10|11 => dout <= reg_request( 95 downto 64) ;
                    when 12|13|14|15 => dout <= reg_request(127 downto 96) ;
                    when 16|17|18|19 => dout(7 downto 0) <= control ;
                    when 20|21|22|23 => dout <= request_session ;
                    when 24|25|26|27 => dout <= session_generation ;
                    when 28|29|30|31 => dout <= (4 => response_active,
                                                   3 => response_overflow,
                                                   2 => pending_response_valid,
                                                   1 => worker_busy,
                                                   0 => request_busy,
                                                   others => '0') ;
                    when others => dout <= (others => '0') ;
                end case ;
            end if ;
        end if ;
    end process ;

    receive_command : process(clock, reset, session_reset)
        type state_t is (WAIT_FOR_MAGIC, CHECK_MAGIC, RECEIVE_COMMAND, INTERRUPT, WAIT_FOR_READ) ;
        variable state : state_t ;
        variable count : natural range 0 to 15 ;
    begin
        if( reset = '1' or session_reset = '1' ) then
            state := WAIT_FOR_MAGIC ;
            count := 0 ;
            irq <= '0' ;
            request_busy <= '0' ;
        elsif( rising_edge(clock) ) then
            if( sin_we = '1' and state /= WAIT_FOR_READ and
                state /= INTERRUPT ) then
                reg_request(count*8+7 downto count*8) <= sin_data ;
            end if ;
            -- Capture worker ownership before the case statement can move a
            -- pending request from WAIT_FOR_READ to WAIT_FOR_MAGIC.
            if( worker_busy = '1' and
                (state = INTERRUPT or state = WAIT_FOR_READ) ) then
                request_busy <= '1';
            end if;
            case state is
                when WAIT_FOR_MAGIC =>
                    if( sin_we = '1' ) then
                        state := CHECK_MAGIC ;
                    end if ;

                when CHECK_MAGIC =>
                    for i in magics'range loop
                        if( magics(i) = reg_request(7 downto 0) ) then
                            state := RECEIVE_COMMAND ;
                            count := count + 1 ;
                            request_session <= session_generation;
                            request_busy <= worker_busy;
                        end if ;
                    end loop ;
                    if( count = 0 ) then
                        state := WAIT_FOR_MAGIC ;
                    end if ;

                when RECEIVE_COMMAND =>
                    if( sin_we = '1' ) then
                        request_busy <= request_busy or worker_busy;
                        if( count < 15 ) then
                            count := count + 1 ;
                        else
                            count := 0 ;
                            -- Capture whether the complete request arrived
                            -- while NIOS owned a synchronous peripheral
                            -- callback. This is independent of later IRQ
                            -- latency and callback return timing.
                            state := INTERRUPT ;
                        end if ;
                    end if ;

                when INTERRUPT =>
                    if( isr_enable = '1' ) then
                        irq <= '1' ;
                        state := WAIT_FOR_READ ;
                    else
                        irq <= '0' ;
                    end if ;

                when WAIT_FOR_READ =>
                    -- NIOS reads the status CSR before consuming the request
                    -- buffer in its ISR. Keep the request latched and IRQ
                    -- asserted until one of the request data words is read;
                    -- a status/session read must not discard the frame.
                    if( read = '1' and
                        to_integer(unsigned(addr)) <= 15 ) then
                        irq <= '0' ;
                        -- The NIOS ISR snapshots request_busy before reading
                        -- this request word, so clear the latch on consumption.
                        request_busy <= '0';
                        state := WAIT_FOR_MAGIC ;
                    end if ;
            end case ;

        end if ;
    end process ;

    -- Synchronize asynchronous inputs
    reg_sin <= rs232_sin when rising_edge( clock ) ;
    sync_sin <= reg_sin when rising_edge( clock ) ;

    -- Receive data from the UART and put it in the FIFO
    receive_from_uart : process(clock, reset, session_reset)
        variable bits           :   std_logic_vector(7 downto 0) ;
        variable bit_count      :   natural range 0 to bits'length ;
        variable clock_count    :   natural range 0 to CLOCKS_PER_BIT ;
        variable sin_delay      :   std_logic ;
    begin
        if( reset = '1' or session_reset = '1' ) then
            receive_fsm <= WAIT_FOR_START ;
            sin_we <= '0' ;
            sin_data <= (others =>'0') ;
            bits := (others =>'0') ;
            bit_count := 0 ;
            clock_count := 0 ;
            sin_delay := '0' ;
        elsif( rising_edge(clock) ) then
            sin_we <= '0' ;
            case receive_fsm is
                -- Wait for the start signal
                when WAIT_FOR_START =>
                    if( sync_sin = '0' and sin_delay = '1' ) then
                        receive_fsm <= CENTER_START ;
                        clock_count := CLOCKS_PER_BIT/2-1 ;
                    end if ;
                    sin_delay := sync_sin ;

                -- Center the start bit
                when CENTER_START =>
                    clock_count := clock_count - 1 ;
                    if( clock_count = 0 ) then
                        receive_fsm <= SAMPLE_DATA ;
                        clock_count := CLOCKS_PER_BIT ;
                        bit_count := bits'length ;
                    end if ;

                -- Sample the bits
                when SAMPLE_DATA =>
                    clock_count := clock_count - 1 ;
                    if( clock_count = 0 ) then
                        bits := sync_sin & bits(bits'high downto 1) ;
                        bit_count := bit_count - 1 ;
                        clock_count := CLOCKS_PER_BIT ;
                        if( bit_count = 0 ) then
                            receive_fsm <= WAIT_FOR_STOP ;
                            sin_data <= bits ;
                            sin_we <= '1' ;
                        end if ;
                    end if ;

                -- Wait for the stop bit
                when WAIT_FOR_STOP =>
                    clock_count := clock_count - 1 ;
                    if( clock_count = 0 ) then
                        receive_fsm <= WAIT_FOR_START ;
                        assert sync_sin = '1' report "STOP bit not found!" severity error ;
                    end if ;

                when others =>
            end case ;
        end if ;
    end process ;

    -- Register outputs
    register_sout : process(clock, reset) is
    begin
        if (reset = '1') then
            rs232_sout <= '1';
        elsif (rising_edge(clock)) then
            if (session_reset = '1') then
                rs232_sout <= '1';
            else
                rs232_sout <= rs232_sout_reg;
            end if;
        end if;
    end process register_sout;

    -- Send data to the UART from the FIFO
    send_to_uart : process(clock, reset, session_reset)
        variable bits           : std_logic_vector(7 downto 0) ;
        variable bit_count      : natural range 0 to bits'length ;
        variable clock_count    : natural range 0 to CLOCKS_PER_BIT ;
    begin
        if( reset = '1' or session_reset = '1' ) then
            send_fsm        <= IDLE ;
            rs232_sout_reg  <= '1' ;
            bit_count       := 0 ;
            clock_count     := 0 ;
            bits            := (others =>'0') ;
        elsif( rising_edge(clock) ) then
            case send_fsm is
                -- Wait until we have something to send
                when IDLE =>
                    rs232_sout_reg  <= '1' ;
                    if( sout_we = '1' ) then
                        send_fsm    <= SEND_START ;
                        bit_count   := bits'length ;
                        clock_count := CLOCKS_PER_BIT ;
                    end if ;

                -- Send the start bit
                when SEND_START =>
                    rs232_sout_reg  <= '0' ;
                    clock_count     := clock_count - 1 ;
                    if( clock_count = 0 ) then
                        send_fsm    <= SEND_DATA ;
                        clock_count := CLOCKS_PER_BIT ;
                    elsif( clock_count = 9 ) then
                        bits        := sout_data ;
                    end if ;
                -- Send the data
                when SEND_DATA =>
                    rs232_sout_reg  <= bits(0) ;
                    clock_count     := clock_count - 1 ;
                    if( clock_count = 0 ) then
                        bits        := '0' & bits(bits'high downto 1) ;
                        bit_count   := bit_count - 1 ;
                        clock_count := CLOCKS_PER_BIT ;
                        if( bit_count = 0 ) then
                            send_fsm <= SEND_STOP ;
                        end if ;
                    end if ;

                -- Send the stop bit
                when SEND_STOP =>
                    rs232_sout_reg  <= '1' ;
                    clock_count     := clock_count - 1 ;
                    if( clock_count = 0 ) then
                        send_fsm    <= IDLE ;
                    end if ;

                -- Weird
                when others =>
                    send_fsm        <= IDLE ;
                    rs232_sout_reg  <= '1' ;
                    bit_count       := 0 ;
                    clock_count     := 0 ;
                    bits            := (others =>'0') ;
            end case ;
        end if ;
    end process ;

end architecture ; -- arch
