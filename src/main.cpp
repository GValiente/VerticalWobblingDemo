//--------------------------------------------------------------------------------
// main.cpp
//--------------------------------------------------------------------------------
// The main function
//--------------------------------------------------------------------------------

#include "bn_core.h"
#include "bn_math.h"
#include "bn_keypad.h"
#include "bn_bg_palette_ptr.h"
#include "bn_regular_bg_ptr.h"
#include "bn_regular_bg_builder.h"
#include "bn_regular_bg_tiles_ptr.h"

#include "bn_regular_bg_items_br_flag.h"
#include "bn_regular_bg_items_us_flag.h"

namespace data
{
    // Flag dimensions
    constexpr int flag_width_pixels = 192;
    constexpr int flag_height_pixels = 128;
    constexpr int flag_width_tiles = flag_width_pixels/8;
    constexpr int flag_height_tiles = flag_height_pixels/8;

    // The flag should be centered
    constexpr int flag_offset_x = (32 - flag_width_tiles)/2;
    constexpr int flag_offset_y = (32 - flag_height_tiles)/2;

    // Allocation numbers
    constexpr int flag_tiles_needed = flag_width_tiles * (flag_height_tiles + 2);

    // Important data to generate the LUT
    constexpr int wave_vertical_amplitude = 4;
    constexpr int wave_horizontal_period = 128;
    constexpr int wave_horizontal_multiplier = 2048 / wave_horizontal_period;
}

namespace arm
{
    // Copies a vertical strip from a background originally formatted to be 32x32 horizontal
    // into a vertically-oriented tile map; will basically copy a tile strip in a contiguous
    // version of memory. This is needed to deal with Butano's formatting tool that only exports
    // tiles in row-major order, not allowing to export in column-major order
    BN_CODE_IWRAM void copy_vertical_tile_strip_8bpp(
            void* dest, const void* src, const uint16_t* map_cells, int num_tiles);
}

class flags_bg
{

public:
    [[nodiscard]] static flags_bg create(const bn::regular_bg_item& bg_item)
    {
        // Allocate tiles and maps needed for the background
        // The 2 multiplying here is because an 8bpp has double the size as two 4bpp tiles,
        // but the function accepts only 4bpp tiles, so we need to multiply
        bn::regular_bg_tiles_ptr tiles = bn::regular_bg_tiles_ptr::allocate(
                    2 * (2 * data::flag_tiles_needed + 1), bn::bpp_mode::BPP_8);
        bn::bg_palette_ptr palette = bg_item.palette_item().create_palette();

        // Create the map and first fill it blank
        bn::regular_bg_map_ptr map = bn::regular_bg_map_ptr::allocate(
                    bn::size(32, 32), bn::move(tiles), bn::move(palette));
        bn::span<bn::regular_bg_map_cell> vram = *map.vram();
        bn::fill(vram.begin(), vram.end(), bn::regular_bg_map_cell());

        // Fill in the map with the proper values
        for(int x = 0; x < data::flag_width_tiles; x++)
        {
            for(int y = 0; y < data::flag_height_tiles + 2; y++)
            {
                int tile_x = data::flag_offset_x + x;
                int tile_y = data::flag_offset_y + y - 1;
                int tile_index = (data::flag_height_tiles + 2) * x + y;
                vram[32 * tile_y + tile_x] = bn::regular_bg_map_cell(tile_index + 1);
            }
        }

        // Now, create the background
        bn::regular_bg_builder builder(bn::move(map));
        return flags_bg(bg_item, builder.release_build());
    }

    [[nodiscard]] const bn::regular_bg_item& bg_item() const
    {
        return *_bg_item;
    }

    void set_bg_item(const bn::regular_bg_item& bg_item)
    {
        _bg_item = &bg_item;
        _transfer();
    }

    void update()
    {
        // Get the dest and the source destinations
        int current_frame = _current_frame;
        int src = current_frame & 1;
        int dst = src ^ 1;

        // Same thing here, since we're doing 8-bpp tiles, we need the 2* in this place
        bn::regular_bg_tiles_ptr bg_tiles = _bg.tiles();
        bn::tile* tiles_base_ptr = bg_tiles.vram()->data();
        bn::tile* tiles_src_ptr = tiles_base_ptr + 2 * (data::flag_tiles_needed * src + 1);
        bn::tile* tiles_dst_ptr = tiles_base_ptr + 2 * (data::flag_tiles_needed * dst + 1);

        constexpr int tiles_to_copy = data::flag_height_tiles + 2;
        constexpr int words_to_copy = 2 * sizeof(bn::tile) * tiles_to_copy / sizeof(uint32_t);
        constexpr int real_tiles_to_copy = (words_to_copy * sizeof(uint32_t)) / sizeof(bn::tile);

        // Here, do the "waving flag" displacement, copying the data to the second frame
        for(int x = 0; x < data::flag_width_tiles; x++)
        {
            // Multiply by 2 here to account that bn::tile represents one 4bpp tile,
            // and we need 2 bn::tiles for one 8bpp tile
            int x_disp = 2 * (data::flag_height_tiles + 2) * x;
            bn::tile* col_src_ptr = tiles_src_ptr + x_disp;
            bn::tile* col_dst_ptr = tiles_dst_ptr + x_disp;

            // Compute the pointer to the base line we will be using here
            // uint64_t is 8 bytes, exactly the size of one tile row
            int d_disp = _displacement(8 * x, current_frame + 1) - _displacement(8 * x, current_frame);
            uint64_t* line_dst_ptr = reinterpret_cast<uint64_t*>(col_dst_ptr) + d_disp;
            bn::memory::copy(*col_src_ptr, real_tiles_to_copy, *reinterpret_cast<bn::tile*>(line_dst_ptr));
        }

        // And update the current frame
        ++_current_frame;
    }

private:
    const bn::regular_bg_item* _bg_item;
    bn::regular_bg_ptr _bg;
    int _current_frame = 0;

    // Get the waving flag displacement based on the position and time
    static int _displacement(int x, int t)
    {
        // This is just to make it beautiful
        int a = data::wave_horizontal_multiplier * (x - t);
        return (data::wave_vertical_amplitude * bn::lut_sin(a & 2047)).round_integer();
    }

    flags_bg(const bn::regular_bg_item& bg_item, bn::regular_bg_ptr&& bg) :
        _bg_item(&bg_item),
        _bg(bn::move(bg))
    {
        _transfer();
    }

    // Transfer the flag's data to the graphics
    void _transfer()
    {
        // Get the necessary data
        const bn::regular_bg_item& flag_item = *_bg_item;
        int dst = _current_frame & 1;
        const bn::tile* flag_tiles_ptr = flag_item.tiles_item().tiles_ref().data();
        const bn::regular_bg_map_cell* flag_map_ptr = flag_item.map_item().cells_ptr();
        // (points to the first non-null tile)
        bn::regular_bg_tiles_ptr bg_tiles = _bg.tiles();
        bn::tile* dest_tiles_ptr = bg_tiles.vram()->data() + 2 * (dst * data::flag_tiles_needed + 1);

        // Now, transfer the tiles using a fast ASM routine
        for(int x = 0; x < data::flag_width_tiles; x++)
        {
            // The 2 needs to be here because bn::tile represents a 4bpp tile,
            // and a 8bpp tile is equivalent to two bn::tile
            bn::tile* tile_ptr = dest_tiles_ptr + 2 * (data::flag_height_tiles + 2) * x;

            // Compute the pointer to the base line we will be using here
            // uint64_t is 8 bytes, exactly the size of one tile row
            int disp = _displacement(8 * x, _current_frame);
            uint64_t* line_ptr = reinterpret_cast<uint64_t*>(tile_ptr + 2) + disp;
            const bn::regular_bg_map_cell* map_ptr =
                    flag_map_ptr + (32 * data::flag_offset_y + data::flag_offset_x + x);
            arm::copy_vertical_tile_strip_8bpp(line_ptr, flag_tiles_ptr, map_ptr, data::flag_height_tiles);
        }

        // Fix the palette
        bn::bg_palette_ptr bg_palette = _bg.palette();
        bg_palette.set_colors(flag_item.palette_item());
    }
};

int main()
{
    bn::core::init();

    flags_bg flags = flags_bg::create(bn::regular_bg_items::br_flag);

    while(true)
    {
        // Toggle the flag when A is pressed
        if (bn::keypad::a_pressed())
        {
            if(flags.bg_item() == bn::regular_bg_items::br_flag)
            {
                flags.set_bg_item(bn::regular_bg_items::us_flag);
            }
            else
            {
                flags.set_bg_item(bn::regular_bg_items::br_flag);
            }
        }

        flags.update();
        bn::core::update();
    }
}
