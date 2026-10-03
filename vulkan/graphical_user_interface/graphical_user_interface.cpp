module;

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

module deren.vulkan.graphical_user_interface;

import deren.utility;
import deren.vulkan.constant_init;

namespace deren::vulkan::gui {
    // ---- gui_content lifecycle (see the module docs: ImGui state lives in ImGui's globals) ----

    gui_content::~gui_content() {
        this->shutdown();
    }

    bool gui_content::init(gui_create_info const& info) {
        if (this->active) {
            return true; // idempotent
        }
        if (info.device == VK_NULL_HANDLE || info.graphics_queue == VK_NULL_HANDLE || info.window == nullptr) {
            deren::utility::log("gui_content: init skipped (incomplete gui_create_info)");
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // NO .ini IS EVER LOADED OR WRITTEN: the panel's layout belongs to the code (the widget list
        // build_panel() pushes, the default panel size in gui_create_info), and a persisted file only ever
        // fought it - a layout recorded by an older build came back and re-arranged a panel whose structure
        // had changed, which reads as "the UI changed for no reason". Setting IniFilename to nullptr disables
        // BOTH the automatic load at the first NewFrame() and the periodic save, so every run starts from the
        // code's own defaults.
        io.IniFilename = nullptr;

        // Display scale: on a monitor with Windows display scaling (the machine this was written on
        // reports 1.50) the framebuffer is LARGER than the logical window. Without telling ImGui, the
        // overlay keeps its 13 px font atlas and every glyph is upscaled by the fractional factor
        // while landing between framebuffer pixels - which is exactly what "the ImGui panel looks
        // soft and twitches" describes. ImGui 1.92 rasterizes fonts dynamically, so pointing
        // style.FontScaleDpi at the monitor's content scale makes the atlas render at the framebuffer
        // resolution while the LAYOUT stays in logical units (window coordinates, which is also what
        // the GLFW backend feeds the mouse in): the panel keeps its size and becomes crisp.
        // io.ConfigDpiScaleFonts additionally lets the backend follow a monitor change.
        if (info.window != nullptr) {
            float scale_x = 1.0f;
            float scale_y = 1.0f;
            glfwGetWindowContentScale(info.window, &scale_x, &scale_y);
            float const scale = std::max(scale_x, scale_y);
            // style.FontScaleDpi is the 1.92 scale factor for "the monitor's content scale" (the
            // docking branch's io.ConfigDpiScaleFonts, which follows a monitor change automatically,
            // is not in this build). Setting it makes the dynamically rasterized font atlas render at
            // the framebuffer resolution while the layout - and the mouse coordinates the GLFW
            // backend feeds - stay in logical window units.
            ImGui::GetStyle().FontScaleDpi = scale;
            deren::utility::log("gui_content: display content scale {:.2f} x {:.2f} - overlay font rasterized at {:.1f} px (logical layout unchanged)", static_cast<double>(scale_x), static_cast<double>(scale_y), 13.0 * static_cast<double>(scale));
        }

        // Platform backend. install_callbacks=true makes imgui chain-call the runtime's own
        // GLFW callbacks (mouse/scroll -> orbit camera) which were registered earlier.
        if (!ImGui_ImplGlfw_InitForVulkan(info.window, /*install_callbacks=*/true)) {
            deren::utility::log("gui_content: ImGui_ImplGlfw_InitForVulkan failed");
            ImGui::DestroyContext();
            return false;
        }

        ImGui_ImplVulkan_InitInfo backend_info = {};
        backend_info.ApiVersion = VK_API_VERSION_1_3;
        backend_info.Instance = info.instance;
        backend_info.PhysicalDevice = info.physical_device;
        backend_info.Device = info.device;
        backend_info.QueueFamily = info.graphics_queue_family;
        backend_info.Queue = info.graphics_queue;
        // backend creates its own descriptor pool (we must not share the runtime's scene pool)
        backend_info.DescriptorPoolSize = 8;
        backend_info.MinImageCount = info.frames_in_flight;
        backend_info.ImageCount = info.frames_in_flight;
        backend_info.UseDynamicRendering = true;
        backend_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT; // the overlay draws on the 1x swapchain
        // The overlay records into the still-open main rendering instance, which carries the
        // scene's depth attachment. Dynamic rendering requires a bound pipeline's
        // depthAttachmentFormat to equal the attachment's format (VUID-vkCmdDrawIndexed-
        // dynamicRenderingUnusedAttachments-08914) unless dynamicRenderingUnusedAttachments is
        // enabled, so declare the scene depth format here - the pipeline still does no depth
        // test/write (the backend's depth stencil state is all-disabled).
        // LIFETIME: pColorAttachmentFormats points at info.color_format (this init() call's
        // argument, alive for the whole call). The struct is only consumed inside
        // ImGui_ImplVulkan_Init, whose vendored backend DEEP-COPIES the format list into its own
        // storage before returning - if that ever stops copying, the pointer must be promoted to
        // a member/static instead of pointing at the transient parameter.
        VkPipelineRenderingCreateInfo const rendering_info = make_rendering_create_info(true, &info.color_format, info.depth_format);
        backend_info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering_info;
        backend_info.CheckVkResultFn = [](VkResult const err) {
            if (err != VK_SUCCESS) {
                deren::utility::log("imgui vulkan backend error: {}", static_cast<int32_t>(err));
            }
        };

        if (!ImGui_ImplVulkan_Init(&backend_info)) {
            deren::utility::log("gui_content: ImGui_ImplVulkan_Init failed");
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        this->active = true;
        this->frames_in_flight = info.frames_in_flight;
        deren::utility::log("gui_content: ImGui overlay initialized (dynamic rendering, 1x swapchain)");
        return true;
    }

    void gui_content::shutdown() {
        if (!this->active) {
            return;
        }
        // No .ini to save on the way out either (see init(): IniFilename is nullptr, so the load and the save
        // are both off and the layout is the code's).
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        this->active = false;
        this->panels.clear();
    }

    bool gui_content::is_active() const noexcept {
        return this->active;
    }

    bool gui_content::wants_mouse() const noexcept {
        return this->active && ImGui::GetIO().WantCaptureMouse;
    }

    // ---- panel management ----

    debug_panel& gui_content::add_panel(std::string title) {
        this->panels.push_back(std::make_unique<debug_panel>(std::move(title)));
        return *this->panels.back();
    }

    void gui_content::remove_panel(debug_panel const& panel) {
        std::erase_if(this->panels, [&panel](std::unique_ptr<debug_panel> const& p) { return p.get() == &panel; });
    }

    void gui_content::set_panel_visible(debug_panel const& panel, bool const visible) { // NOLINT
        // debug_panel stores its open flag in is_open; find the panel and update it
        for (auto const& p : this->panels) {
            if (p.get() == &panel) {
                p->set_open(visible);
                return;
            }
        }
    }

    std::size_t gui_content::panel_count() const noexcept {
        return this->panels.size();
    }

    void gui_content::new_frame() const {
        if (!this->active) {
            return;
        }
        ImGui_ImplGlfw_NewFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
    }

    void gui_content::record(VkCommandBuffer const cmd) { // NOLINT
        if (!this->active) {
            return;
        }
        for (auto const& panel : this->panels) {
            if (panel->open()) {
                panel->draw();
            }
        }
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    void gui_content::on_swapchain_recreated() const {
        if (!this->active) {
            return;
        }
        // swapchain image count may have changed; the backend's per-frame buffers stay sized to
        // the frames-in-flight count captured at init(), so only sync the min-image hint.
        ImGui_ImplVulkan_SetMinImageCount(this->frames_in_flight);
    }

    // ---- debug_panel ----

    debug_panel::debug_panel(std::string title)
        : title{std::move(title)} {
    }

    void debug_panel::push_back(std::unique_ptr<widget> item) {
        this->items.push_back(std::move(item));
    }

    void debug_panel::pop_back() {
        if (!this->items.empty()) {
            this->items.pop_back();
        }
    }

    void debug_panel::clear() {
        this->items.clear();
    }

    bool debug_panel::empty() const noexcept {
        return this->items.empty();
    }

    std::size_t debug_panel::size() const noexcept {
        return this->items.size();
    }

    std::string const& debug_panel::get_title() const noexcept {
        return this->title;
    }

    void debug_panel::set_open(bool const open) noexcept {
        this->is_open = open;
    }

    bool debug_panel::open() const noexcept {
        return this->is_open;
    }

    void debug_panel::set_default_size(float const width, float const height) noexcept {
        this->default_width = width;
        this->default_height = height;
    }

    void debug_panel::draw() {
        if (this->default_width > 0.0f && this->default_height > 0.0f) {
            ImGui::SetNextWindowSize(ImVec2(this->default_width, this->default_height), ImGuiCond_FirstUseEver);
        }
        if (!ImGui::Begin(this->title.c_str(), &this->is_open)) {
            ImGui::End();
            return;
        }
        for (auto const& item : this->items) {
            if (item == nullptr) {
                continue;
            }
            // An empty visible_when means "always drawn" (most widgets are unconditional); the
            // predicate lets the panel offer only what the current render path can actually use.
            if (item->visible_when && !item->visible_when()) {
                continue;
            }
            item->draw();
        }
        ImGui::End();
    }

    // ---- widgets ----

    label_widget::label_widget(std::string text)
        : text_fn{[t = std::move(text)]() { return t; }} { // copies per frame (stable text)
    }

    label_widget::label_widget(std::function<std::string()> text_fn)
        : text_fn{std::move(text_fn)} {
    }

    void label_widget::draw() {
        if (this->text_fn) {
            ImGui::TextUnformatted(this->text_fn().c_str());
        }
    }

    checkbox_widget::checkbox_widget(std::string label, bool* value, std::function<void(bool)> on_change)
        : label{std::move(label)}
        , value{value}
        , on_change{std::move(on_change)} {
    }

    void checkbox_widget::draw() {
        if (this->value == nullptr) {
            return;
        }
        bool const before = *this->value;
        if (ImGui::Checkbox(this->label.c_str(), this->value) && before != *this->value && this->on_change) {
            this->on_change(*this->value);
        }
    }

    slider_widget::slider_widget(std::string label, float* value, float min, float max, std::function<void(float)> on_change)
        : label{std::move(label)}
        , value{value}
        , min{min}
        , max{max}
        , on_change{std::move(on_change)} {
    }

    void slider_widget::draw() {
        if (this->value == nullptr) {
            return;
        }
        float const before = *this->value;
        if (ImGui::SliderFloat(this->label.c_str(), this->value, this->min, this->max) && before != *this->value && this->on_change) {
            this->on_change(*this->value);
        }
    }

    vec3_widget::vec3_widget(std::string label, float* value, float const speed, std::function<void()> on_change)
        : label{std::move(label)}
        , value{value}
        , speed{speed}
        , on_change{std::move(on_change)} {
    }

    void vec3_widget::draw() {
        if (this->value == nullptr) {
            return;
        }
        // snapshot the xyz triplet to detect whether the drag actually changed anything
        std::array<float, 3> const before = {this->value[0], this->value[1], this->value[2]};
        if (ImGui::DragFloat3(this->label.c_str(), this->value, this->speed)) {
            bool changed = false;
            for (int32_t i = 0; i < 3; ++i) {
                changed = changed || this->value[i] != before[static_cast<std::size_t>(i)];
            }
            if (changed && this->on_change) {
                this->on_change();
            }
        }
    }

    combo_widget::combo_widget(std::string label, std::vector<std::string> items, int32_t* current_item, std::function<void(int32_t)> on_change)
        : label{std::move(label)}
        , items{std::move(items)}
        , current_item{current_item}
        , on_change{std::move(on_change)} {
    }

    void combo_widget::draw() {
        if (this->current_item == nullptr || this->items.empty()) {
            return;
        }
        *this->current_item = std::clamp(*this->current_item, 0, static_cast<int32_t>(this->items.size()) - 1);
        int32_t const before = *this->current_item;
        if (ImGui::BeginCombo(this->label.c_str(), this->items[static_cast<std::size_t>(*this->current_item)].c_str())) {
            for (int32_t i = 0; i < static_cast<int32_t>(this->items.size()); ++i) {
                bool const selected = i == *this->current_item;
                if (ImGui::Selectable(this->items[static_cast<std::size_t>(i)].c_str(), selected)) {
                    *this->current_item = i;
                }
            }
            ImGui::EndCombo();
        }
        if (before != *this->current_item && this->on_change) {
            this->on_change(*this->current_item);
        }
    }
} // namespace deren::vulkan::gui
