-- fx3_gpif waterlines against the real FIFO depth: room for one DMA block
-- (TX) and "cannot fit one more block" (RX), at 2048/4096/8192/16384 words,
-- both USB speeds, the edges of each threshold and the full flag.
library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

library nuand;
    use nuand.fx3_gpif_p.all;

entity fx3_gpif_waterline_tb is
end entity;

architecture test of fx3_gpif_waterline_tb is
begin
    process
        type depths_t is array (natural range <>) of natural;
        constant DEPTHS : depths_t := (2048, 4096, 8192, 16384);
        type bufs_t is array (natural range <>) of natural;
        constant BUFS : bufs_t := (GPIF_BUF_SIZE_SS, GPIF_BUF_SIZE_HS);
        -- usedw as the zero-extended count of a 14-bit port
        function u(n : natural) return std_logic_vector is
        begin
            return std_logic_vector(to_unsigned(n mod 2**14, 14));
        end function;
        variable d, b : natural;
    begin
        for di in DEPTHS'range loop
            for bi in BUFS'range loop
                d := DEPTHS(di);
                b := BUFS(bi);
                if d > b then
                    assert fifo_has_room(u(0), '0', d, b)
                        report "empty FIFO of " & integer'image(d) & " has no room for " & integer'image(b)
                        severity failure;
                    assert fifo_has_room(u(d - b - 1), '0', d, b)
                        report "room lost one word early at depth " & integer'image(d)
                        severity failure;
                    assert not fifo_has_room(u(d - b), '0', d, b)
                        report "room claimed with only one block free at depth " & integer'image(d)
                        severity failure;
                    assert not fifo_is_critical(u(d - b - 1), '0', d, b)
                        report "critical one word early at depth " & integer'image(d)
                        severity failure;
                    assert fifo_is_critical(u(d - b), '0', d, b)
                        report "not critical with one block free at depth " & integer'image(d)
                        severity failure;
                else
                    assert not fifo_has_room(u(0), '0', d, b)
                        report "FIFO of " & integer'image(d) & " claims room for a block of " & integer'image(b)
                        severity failure;
                end if;
                -- Full reads usedw 0.
                assert not fifo_has_room(u(0), '1', d, b)
                    report "full FIFO of " & integer'image(d) & " claims room"
                    severity failure;
                assert fifo_is_critical(u(0), '1', d, b)
                    report "full FIFO of " & integer'image(d) & " not critical"
                    severity failure;
            end loop;
        end loop;
        report "fx3_gpif_waterline_tb: PASS";
        wait;
    end process;
end architecture;
