#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <cstdint>

#include <SDL.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/random.hpp>
#include <glm/gtx/transform.hpp>
#include "opengl.h"

#include "program.h"
#include "bufferobject.h"
#include "mesh.h"
#include "fpscamera.h"
#include "list.h"
#include "quadtree.h"
#include "renderqueue.h"

#define STBI_HEADER_FILE_ONLY
#include "stb_image.c"


#ifdef WIN32
#include <Windows.h>
#endif

#define die(fmt, ...) \
do { \
    \
    printf(fmt "\n", __VA_ARGS__); \
    exit(1); \
} while (0)





static RenderQueue renderqueue;
static Program::Ref ship_program;
static Program::Ref qtree_program;
static Mesh::Ref ship_mesh;

static mat4 projection_matrix;
static mat4 view_matrix;
static vec3 light_dir;
static vec3 cursor_pos;





class World;

class Body : public QuadTree::Object {
public:
    vec3 pos;
    vec3 vel;


    ListLink world_link;

    Body() {}

    virtual vec2 qtree_position() {
        return vec2(pos);
    }

    virtual void update(World &world) {}
    virtual void render(RenderQueue *renderer) {}

private:
    Body(const Body &);
    Body &operator=(const Body &);
};


class World {
public:
    typedef List<Body, &Body::world_link> BodyList;

    BodyList bodies;
    QuadTree quadtree;
    float dt;

    World() : quadtree(Rect(-1000, -1000, 1000, 1000), 7) {
    }

    ~World() {
        while (!bodies.empty())
            delete bodies.front();
    }

    void add_body(Body *body) {
        bodies.push_back(body);
        quadtree.insert(body);
    }

    void update() {
        for (Body *body : bodies)
            body->update(*this);
    }

    void render() {
        for (Body *body : bodies)
            body->render(&renderqueue);
    }

private:
    World(const World &);
    World &operator=(const World &);
};




struct UpdateContext {
    UpdateContext(World &w) : world(w), quadtree(w.quadtree) {}

    World &world;
    QuadTree &quadtree;
    std::vector<QuadTree::Object *> neighbours;
};

vec3 limit(vec3 v, float len) {
    if (glm::length(v) > len)
        return glm::normalize(v) * len;
    return v;
}

class Boid : public Body {
public:
    float maxspeed;
    float maxforce;
    int team;

    void update(World &world) {
        UpdateContext context(world);

        context.quadtree.query(vec2(pos), 50.0f, context.neighbours);
        //printf("context.neighbours: %d\n", context.neighbours.size());

        vec3 acc(0, 0, 0);
        
        acc += separation(context) * 1.5f;
        acc += alignment(context) * 1.0f;
        acc += cohesion(context) * 1.0f;
        
        acc += planehug(context) * 1.0f;
        acc += zseparation(context) * 1.0f;
        
        acc += seek(cursor_pos) * 1.0f;

        vel += acc * world.dt;
        vel = limit(vel, maxspeed);

        pos += vel * world.dt;
        //pos.z = 0;
        
        qtree_update();
    }

    vec3 planehug(UpdateContext &context) {
        vec3 target = pos;
        target.z = 0;
        return steer(target - pos);
    }

    vec3 zseparation(UpdateContext &context) {
        float sep = 20.0f;
        vec3 sum(0, 0, 0);
        int count = 0;
        for (auto obj : context.neighbours) {
            Boid *b = static_cast<Boid *>(obj);
            if (b == this) continue;
            vec3 d = pos - b->pos;
            float len = glm::length(d);
            if (len > sep || len <= 0.00001f) continue;
            //d = glm::normalize(d);
            float dz = d.z;
            if (dz == 0.0f)
                dz = glm::dot(glm::normalize(vel), glm::normalize(b->vel));
            dz /= fabsf(dz);
            dz /= len;
            sum += vec3(0, 0, dz);
            ++count;
        }
        if (count == 0)
            return vec3(0, 0, 0);
        sum /= (float)count;
        return steer(sum);
    }

    vec3 separation(UpdateContext &context) {
        float sep = 20.0f;
        vec3 sum(0, 0, 0);
        int count = 0;
        for (auto obj : context.neighbours) {
            Boid *b = static_cast<Boid *>(obj);
            if (b == this) continue;
            vec3 d = pos - b->pos;
            float len = glm::length(d);
            if (len > sep || len <= 0.00001f) continue;
            d = glm::normalize(d);
            d /= len;
            sum += d;
            ++count;
        }
        if (count == 0)
            return vec3(0, 0, 0);
        sum /= (float)count;
        return steer(sum);
    }

    vec3 alignment(UpdateContext &context) {
        float neighbordist = 50;
        vec3 sum(0, 0, 0);
        int count = 0;
        for (auto obj : context.neighbours) {
            Boid *b = static_cast<Boid *>(obj);
            if (b == this) continue;
            if (b->team != team) continue;
            vec3 d = pos - b->pos;
            float dist = glm::length(d);
            if (dist > neighbordist) continue;
            sum += b->vel;
            ++count;
        }
        if (count == 0)
            return vec3(0, 0, 0);
        sum /= (float)count;
        return steer(sum);
    }

    vec3 cohesion(UpdateContext &context) {
        float neighbordist = 50;
        vec3 sum(0, 0, 0);
        int count = 0;
        for (auto obj : context.neighbours) {
            Boid *b = static_cast<Boid *>(obj);
            if (b == this) continue;
            if (b->team != team) continue;
            vec3 d = pos - b->pos;
            float len = glm::length(d);
            if (len > neighbordist) continue;
            sum += b->pos;
            ++count;
        }
        if (count == 0)
            return vec3(0, 0, 0);
        sum /= (float)count;
        return seek(sum);
    }

    vec3 seek(vec3 target) {
        return steer(target - pos);
    }

    vec3 steer(vec3 dir) {
        float len = glm::length(dir);
        if (len < 0.000001f)
            return vec3(0, 0, 0);
        dir *= maxspeed / len;
        return limit(dir - vel, maxforce);
    }

    mat4 calc_rotation_matrix() {
        vec3 temp_up(0, 0, 1);
        vec3 forward(glm::normalize(-vel));
        vec3 right(glm::cross(forward, temp_up));
        vec3 up(glm::cross(right, forward));

        mat4 m;
        m[0] = vec4(right, 0);
        m[1] = vec4(forward, 0);
        m[2] = vec4(up, 0);
        return m;
    }

    void render(RenderQueue *renderqueue) {
        mat4 model = glm::translate(pos) * calc_rotation_matrix();
        mat4 vm = view_matrix * model;
        mat4 pvm = projection_matrix * vm;
        mat3 normal = glm::inverseTranspose(mat3(vm));

        auto cmd = renderqueue->add_command(ship_program, ship_mesh);
        cmd->add_uniform("m_pvm", pvm);
        cmd->add_uniform("m_vm", vm);
        cmd->add_uniform("m_normal", normal);
        cmd->add_uniform("light_dir", light_dir);
        if (team == 0) {
            cmd->add_uniform("mat_ambient", vec4(0.25f, 0.25f, 0.25f, 1));
            cmd->add_uniform("mat_diffuse", vec4(0.4f, 0.4f, 0.4f, 1));
            cmd->add_uniform("mat_specular", vec4(0.774597f, 0.774597f, 0.774597f, 1));
            cmd->add_uniform("mat_shininess", 76.8f);
        } else {
            cmd->add_uniform("mat_ambient", vec4(0.329412f, 0.223529f, 0.027451f, 1.0f));
            cmd->add_uniform("mat_diffuse", vec4(0.780392f, 0.568627f, 0.113725f, 1.0f));
            cmd->add_uniform("mat_specular", vec4(0.992157f, 0.941176f, 0.807843f, 1.0f));
            cmd->add_uniform("mat_shininess", 27.89743616f);
        }
    }
};







struct MeshFileHeader {
    uint32_t fourcc;
    uint32_t version;

    uint32_t num_vertices;
    uint32_t num_indices;

    uint32_t have_normals;
    uint32_t have_tangents;
    uint32_t have_bitangents;
    uint32_t num_texcoord_sets;
    uint32_t num_color_sets;
};



#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>


static Mesh::Ref do_load_mesh(aiMesh *aimesh) {
    std::vector<GLfloat> verts;
    std::vector<GLuint> indices;

    assert(aimesh->HasPositions());
    assert(aimesh->HasNormals());

    for (unsigned int i = 0; i < aimesh->mNumVertices; ++i) {
        aiVector3D v = aimesh->mVertices[i];
        verts.push_back(v.x);
        verts.push_back(v.y);
        verts.push_back(v.z);

        aiVector3D n = aimesh->mNormals[i];
        verts.push_back(n.x);
        verts.push_back(n.y);
        verts.push_back(n.z);
    }

    for (unsigned int i = 0; i < aimesh->mNumFaces; ++i) {
        aiFace f = aimesh->mFaces[i];
        if (f.mNumIndices != 3)
            return 0;
        indices.push_back(f.mIndices[0]);
        indices.push_back(f.mIndices[1]);
        indices.push_back(f.mIndices[2]);
    }

    VertexFormat::Ref format = VertexFormat::create();
    format->add(VertexFormat::Position, 0, 3, GL_FLOAT);
    format->add(VertexFormat::Normal, 1, 3, GL_FLOAT);

    Mesh::Ref mesh = Mesh::create(GL_TRIANGLES, 1);
    mesh->bind();

    BufferObject::Ref index_buffer = BufferObject::create();
    index_buffer->bind(GL_ELEMENT_ARRAY_BUFFER);
    index_buffer->data(sizeof(indices[0])*indices.size(), &indices[0]);
    mesh->set_index_buffer(index_buffer, indices.size(), GL_UNSIGNED_INT);

    BufferObject::Ref vertex_buffer = BufferObject::create();
    vertex_buffer->bind(GL_ARRAY_BUFFER);
    vertex_buffer->data(sizeof(verts[0])*verts.size(), &verts[0]);
    mesh->set_vertex_buffer(0, vertex_buffer, format);

    mesh->unbind();
    return mesh;
}

Mesh::Ref load_mesh(const std::string &filename) {
    Assimp::Importer importer;

    const aiScene* scene = importer.ReadFile(filename,
                                             aiProcess_Triangulate |
                                             aiProcess_SortByPType |
                                             aiProcess_JoinIdenticalVertices |
                                             aiProcess_OptimizeMeshes |
                                             aiProcess_OptimizeGraph |
                                             aiProcess_PreTransformVertices |
                                             aiProcess_GenSmoothNormals
                                             );

    if (!scene) {
        printf("import error: %s\n", importer.GetErrorString());
        return 0;
    }

    printf("num meshes: %d\n\n", scene->mNumMeshes);

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        aiMesh *aimesh = scene->mMeshes[i];
        printf("  %s -\tverts: %d,\tfaces: %d,\tmat: %d,\thas colors: %d\n", aimesh->mName.C_Str(), aimesh->mNumVertices, aimesh->mNumFaces, aimesh->mMaterialIndex, aimesh->HasVertexColors(0));

        Mesh::Ref mesh = do_load_mesh(aimesh);
        if (!mesh)
            continue;
        return mesh;
    }
    return 0;
}








static void LoadTriangle() {
    ship_mesh = load_mesh("../data/meshes/harv.ply");

    ship_program = Program::create();
    ship_program->attach(Shader::load(GL_VERTEX_SHADER, "../data/shaders/simple.vert"));
    ship_program->attach(Shader::load(GL_FRAGMENT_SHADER, "../data/shaders/simple.frag"));
    ship_program->attrib("in_pos", 0);
    ship_program->attrib("in_normal", 1);
    ship_program->link();
    ship_program->detach_all();

    qtree_program = Program::create();
    qtree_program->attach(Shader::load(GL_VERTEX_SHADER, "../data/shaders/pos.vert"));
    qtree_program->attach(Shader::load(GL_FRAGMENT_SHADER, "../data/shaders/color.frag"));
    qtree_program->attrib("in_pos", 0);
    qtree_program->link();
    qtree_program->detach_all();
}




int main(int argc, char *argv[]) {
#ifdef WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CON", "w", stdout);
        freopen("CON", "w", stderr);
    }
#endif
    printf("Starting...\n");

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        die("SDL_Init() error: %s", SDL_GetError());

    bool ok = true;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 8) == 0;
    ok = ok && SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1) == 0;
    if (!ok)
        die("SDL_GL_SetAttribute() error: %s", SDL_GetError());

    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) < 0)
        die("SDL_GetDesktopDisplayMode() error: %s", SDL_GetError());
    //mode.w = 1920, mode.h = 1080;

    SDL_Window *window = SDL_CreateWindow(
        "Test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        mode.w, mode.h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
        );
    if (!window)
        die("SDL_CreateWindow() error: %s", SDL_GetError());

    if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) < 0)
    	die("SDL_SetWindowFullscreen() error: %s", SDL_GetError());

    SDL_GLContext glcontext = SDL_GL_CreateContext(window);
    if (!glcontext)
        die("SDL_GL_CreateContext() error: %s", SDL_GetError());

    glewExperimental = GL_TRUE;
    GLenum status = glewInit();
    if (status != GLEW_OK)
        die("glewInit() error: %s", glewGetErrorString(status));

    if (!GLEW_VERSION_3_2)
        die("OpenGL 3.2 API is not available.");


    if (SDL_GL_SetSwapInterval(-1) < 0) {
        printf("SDL_GL_SetSwapInterval(-1) failed: %s\n", SDL_GetError());
        if (SDL_GL_SetSwapInterval(1) < 0)
            printf("SDL_GL_SetSwapInterval(1) failed: %s\n", SDL_GetError());
    }

    glEnable(GL_MULTISAMPLE);

    try {
        LoadTriangle();
    } catch (const std::exception &e) {
        die("error: %s", e.what());
    }


    const int qtree_max_vertexes = 16000;
    auto qtree_mesh = Mesh::create(GL_LINES, 1);
    qtree_mesh->bind();
    auto qtree_fmt(VertexFormat::create());
    qtree_fmt->add(VertexFormat::Position, 0, 2, GL_FLOAT);
    auto qtree_buf(BufferObject::create());
    qtree_buf->bind(GL_ARRAY_BUFFER);
    qtree_buf->data(sizeof(vec2)*qtree_max_vertexes, nullptr, GL_STREAM_DRAW);
    qtree_mesh->set_vertex_buffer(0, qtree_buf, qtree_fmt);
    qtree_mesh->unbind();
    std::vector<vec2> qtree_lines;

    const int vlines_max_vertexes = 16000;
    auto vlines_mesh = Mesh::create(GL_LINES, 1);
    vlines_mesh->bind();
    auto vlines_fmt(VertexFormat::create());
    vlines_fmt->add(VertexFormat::Position, 0, 3, GL_FLOAT);
    auto vlines_buf(BufferObject::create());
    vlines_buf->bind(GL_ARRAY_BUFFER);
    vlines_buf->data(sizeof(vec2)*vlines_max_vertexes, nullptr, GL_STREAM_DRAW);
    vlines_mesh->set_vertex_buffer(0, vlines_buf, vlines_fmt);
    vlines_mesh->unbind();
    std::vector<vec3> vlines_lines;

    //SDL_SetRelativeMouseMode(SDL_TRUE);

    World world;

    for (int i = 0; i < 40; ++i) {
        Boid *b = new Boid();
        b->pos = vec3(glm::diskRand(200.0f), 0.0f);
        b->vel = vec3(glm::diskRand(10.0f), 0.0f);
        b->maxspeed = 40;
        b->maxforce = 1;
        b->team = rand() % 2;
        world.add_body(b);
    }

    vec3 camera_pos(0, -5, 20);
    vec3 camera_right(1, -1, 0);
    vec3 camera_forward(1, 1, 0);
    float aspect_ratio = (float)mode.w / (float)mode.h;

    /*FPSCamera camera;
    camera.set_pos(camera_pos);
    camera.look_at(camera_pos + vec3(0.0f, 1.0f, -2.0f));*/

    light_dir = vec3(1, 1, 3);

    const Uint8 *keys = SDL_GetKeyboardState(0);
    Uint32 prevticks = SDL_GetTicks();
    bool running = true;

    while (running) {
        //////////////////////////////////////////////////////////////////////////////////////////////////
        // Updating:
        //////////////////////////////////////////////////////////////////////////////////////////////////

        Uint32 ticks = SDL_GetTicks();
        Uint32 tickdiff = ticks - prevticks;
        prevticks = ticks;
        float dt = (float)tickdiff / 1000.0f;
        world.dt = dt;

        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);

#define ORTHO 1
#if ORTHO
        float viewport_radius = camera_pos.z;
        projection_matrix = glm::ortho(-viewport_radius*aspect_ratio,
                                       viewport_radius*aspect_ratio,
                                       -viewport_radius,
                                       viewport_radius,
                                       -10000.0f,
                                       10000.0f);
#else
        projection_matrix = glm::perspective(45.0f, aspect_ratio, 0.1f, 10000.0f);
#endif

        view_matrix = glm::lookAt(camera_pos,
                                  camera_pos + vec3(1, 1, -1),
                                  vec3(0, 0, 1));
        //view_matrix = camera.view_matrix();

        light_dir = glm::normalize(glm::angleAxis(dt*10.0f, vec3(0, 0, 1)) * light_dir);

        {
            vec3 P0 = glm::unProject(vec3(mx, mode.h - my - 1, 0), view_matrix, projection_matrix, vec4(0, 0, mode.w, mode.h));
            vec3 P1 = glm::unProject(vec3(mx, mode.h - my - 1, 1), view_matrix, projection_matrix, vec4(0, 0, mode.w, mode.h));
            vec3 V = glm::normalize(P1 - P0);
            vec3 N(0, 0, 1);
            float d = 0;
            float t = -(glm::dot(P0, N) + d) / glm::dot(V, N);
            cursor_pos = P0 + V * t;
        }

        world.update();
        //world.bodies.front()->pos = cursor_pos;


        //////////////////////////////////////////////////////////////////////////////////////////////////
        // Rendering:
        //////////////////////////////////////////////////////////////////////////////////////////////////

        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        {
            // update quad tree outline mesh
            qtree_lines.clear();
            world.quadtree.gather_outlines(qtree_lines);
            /*for (auto b : world.bodies) {
                qtree_lines.pu
            }*/
            if (qtree_lines.size() > qtree_max_vertexes)
                qtree_lines.resize(qtree_max_vertexes);
            qtree_buf->bind(GL_ARRAY_BUFFER);
            qtree_buf->write(0, sizeof(qtree_lines[0])*qtree_lines.size(), &qtree_lines[0]);
            qtree_buf->unbind();
            qtree_mesh->set_num_vertexes(qtree_lines.size());

            // render it (with depth writes off)
            auto cmd = renderqueue.add_command(qtree_program, qtree_mesh);
            cmd->add_uniform("m_pvm", projection_matrix * view_matrix);
            cmd->add_uniform("color", vec4(0.2f, 0.2f, 0.2f, 1));
            glDepthMask(GL_FALSE);
            renderqueue.flush();
            glDepthMask(GL_TRUE);
        }

        {
            vlines_lines.clear();
            for (auto b : world.bodies) {
                vec3 pos = b->pos;
                pos.z = 0;
                vlines_lines.push_back(pos);
                vlines_lines.push_back(b->pos);
            }
            if (vlines_lines.size() > vlines_max_vertexes)
                vlines_lines.resize(vlines_max_vertexes);
            vlines_buf->bind(GL_ARRAY_BUFFER);
            vlines_buf->write(0, sizeof(vlines_lines[0])*vlines_lines.size(), &vlines_lines[0]);
            vlines_buf->unbind();
            vlines_mesh->set_num_vertexes(vlines_lines.size());

            // render it (with depth writes off)
            auto cmd = renderqueue.add_command(qtree_program, vlines_mesh);
            cmd->add_uniform("m_pvm", projection_matrix * view_matrix);
            cmd->add_uniform("color", vec4(0.5f, 0.5f, 0.5f, 1));
            renderqueue.flush();
        }

        world.render();
        renderqueue.flush();
        
        SDL_GL_SwapWindow(window);


        //////////////////////////////////////////////////////////////////////////////////////////////////
        // Event handling:
        //////////////////////////////////////////////////////////////////////////////////////////////////

        float speed = 1.0;
        float sensitivity = 0.01f;

        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A] || mx == 0)
            camera_pos -= camera_right*camera_pos.z*dt*speed;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D] || mx == mode.w - 1)
            camera_pos += camera_right*camera_pos.z*dt*speed;
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W] || my == 0)
            camera_pos += camera_forward*camera_pos.z*dt*speed;
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S] || my == mode.h - 1)
            camera_pos -= camera_forward*camera_pos.z*dt*speed;
        /*if (keys[SDL_SCANCODE_SPACE])
            camera_pos.y += dt*speed;
        if (keys[SDL_SCANCODE_C])
            camera_pos.y -= dt*speed;*/
        /*if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A])
            camera.strafe(-dt*speed);
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D])
            camera.strafe(dt*speed);
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W])
            camera.step(dt*speed);
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S])
            camera.step(-dt*speed);
        if (keys[SDL_SCANCODE_SPACE])
            camera.rise(dt*speed);
        if (keys[SDL_SCANCODE_C])
            camera.rise(-dt*speed);*/

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_KEYDOWN:
                break;
            case SDL_KEYUP:
                if (event.key.keysym.sym == SDLK_ESCAPE)
                    running = false;
                break;
            case SDL_MOUSEMOTION:
                //camera.rotate((float)event.motion.xrel * -sensitivity, (float)event.motion.yrel * -sensitivity);
                break;
            case SDL_MOUSEWHEEL:
                camera_pos.z += -0.4f * event.wheel.y * camera_pos.z;
                if (camera_pos.z < 1.0f)
                    camera_pos.z = 1.0f;
                if (camera_pos.z > 1000.0f)
                    camera_pos.z = 1000.0f;
                break;
            case SDL_QUIT:
                running = false;
                break;
            }
        }
    }

    ship_program = 0;
    qtree_program = 0;
    ship_mesh = 0;

    SDL_GL_DeleteContext(glcontext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("Done.\n");
    return 0;
}


/*
TODO: implement object pool. use it for bodies and associated data (like behaviors)
*/

