#ifndef __EVENTS_HPP__
#define __EVENTS_HPP__

namespace ultramodern {
    namespace events {
        struct callbacks_t {
            using vi_callback_t = void();
            using gfx_init_callback_t = void();
            using gfx_update_callback_t = void();

            /**
             * Called in each VI.
             */
            vi_callback_t* vi_callback;

            /**
             * Called before entering the gfx main loop.
             */
            gfx_init_callback_t* gfx_init_callback;

            /**
             * Called on the gfx thread before each screen update.
             */
            gfx_update_callback_t* gfx_update_callback;
        };

        void set_callbacks(const callbacks_t& callbacks);
        void request_screen_update();
    }
}

#endif
