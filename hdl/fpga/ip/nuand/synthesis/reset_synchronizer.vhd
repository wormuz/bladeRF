library ieee ;
    use ieee.std_logic_1164.all ;

entity reset_synchronizer is
  generic (
    INPUT_LEVEL     :       std_logic   := '1' ;
    OUTPUT_LEVEL    :       std_logic   := '1'
  ) ;
  port (
    clock           :   in  std_logic ;
    async           :   in  std_logic ;
    sync            :   out std_logic
  ) ;
end entity ;

architecture arch of reset_synchronizer is

    attribute ALTERA_ATTRIBUTE  : string;
    attribute PRESERVE          : boolean;

    signal reg0, reg1   : std_logic;

    -- This block is not a data synchronizer: `async` is the asynchronous
    -- RESET of all three flops (see the process below), asserted immediately
    -- and released synchronously. So the thing that must be cut is the reset
    -- path into the flops, which is exactly what Altera's own reset
    -- controller does -- it false-paths the aclr/clrn pins and nothing else.
    --
    -- The previous form cut by register instead:
    --     set_false_path -to [get_registers {*reset_synchronizer:*|*}]
    -- and the trailing wildcard swept up reg1 and sync too, so the
    -- reg0 -> reg1 -> sync hops were excluded from analysis as well. Those
    -- hops are the settling time the synchronizer exists to provide; they are
    -- ordinary synchronous paths and must stay timed.
    --
    -- Guard attributes match Altera's std_synchronizer; see synchronizer.vhd
    -- for why DONT_MERGE_REGISTER matters when a design holds 22 of these.
    attribute ALTERA_ATTRIBUTE of arch  : architecture is "-name SDC_STATEMENT ""set_false_path -to [get_pins -compatibility_mode -nowarn {*reset_synchronizer:*|*|clrn}] """;
    attribute ALTERA_ATTRIBUTE of reg0  : signal is "-name SYNCHRONIZER_IDENTIFICATION ""FORCED IF ASYNCHRONOUS"" ; -name DONT_MERGE_REGISTER ON ; -name PRESERVE_REGISTER ON ; -name ADV_NETLIST_OPT_ALLOWED NEVER_ALLOW";
    attribute ALTERA_ATTRIBUTE of reg1  : signal is "-name SYNCHRONIZER_IDENTIFICATION ""FORCED IF ASYNCHRONOUS"" ; -name DONT_MERGE_REGISTER ON ; -name PRESERVE_REGISTER ON ; -name ADV_NETLIST_OPT_ALLOWED NEVER_ALLOW";
    attribute PRESERVE of reg0          : signal is TRUE;
    attribute PRESERVE of reg1          : signal is TRUE;
    attribute PRESERVE of sync          : signal is TRUE;

begin

    synchronize : process( clock, async )
    begin
        if( async = INPUT_LEVEL ) then
            sync <= OUTPUT_LEVEL ;
            reg0 <= OUTPUT_LEVEL ;
            reg1 <= OUTPUT_LEVEL ;
        elsif( rising_edge( clock ) ) then
            sync <= reg1 ;
            reg1 <= reg0 ;
            reg0 <= not OUTPUT_LEVEL ;
        end if ;
    end process ;

end architecture ;

