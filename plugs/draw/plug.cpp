#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <vector>
#include <memory>

#include <raylib.h>
#include <raymath.h>
#include "env.h"
#include "interpolators.h"
#include "plug.h"

#define FONT_SIZE 68

class Task {
public:
    virtual ~Task() = default;
    virtual bool update(Env env) = 0;
};

class Seq: public Task {
public:
    Seq(std::initializer_list<Task*> tasks):
        it(0),
        tasks(tasks)
    {}

    virtual ~Seq() override {
        for (auto task: tasks) {
            delete task;
        }
    }

    bool done() const
    {
        return it >= tasks.size();
    }

    virtual bool update(Env env) override
    {
        if (done()) return true;
        if (tasks[it]->update(env)) it += 1;
        return done();
    }

protected:
    size_t it;
    std::vector<Task*> tasks;
};

class Wait: public Task {
public:
    Wait(float duration):
        started(false),
        cursor(0.0f),
        duration(duration)
    {}

    bool done() const
    {
        return cursor > duration;
    }

    float interp() const
    {
        if (duration <= 0.0f) {
            return 0.0f;
        } else {
            return cursor/duration;
        }
    }

    virtual bool update(Env env) override
    {
        if (done()) return true;
        if (!started) started = true;
        cursor += env.delta_time;
        return done();
    }

protected:
    bool started;
    float cursor;
    float duration;
};

class Move_Vec2: public Wait {
public:
    Move_Vec2(Vector2 *place, Vector2 target, float duration):
        Wait(duration),
        place(place),
        start(Vector2()),
        target(target)
    {}

    virtual bool update(Env env) override
    {
        if (!started) start = *place;
        bool finished = Wait::update(env);
        *place = Vector2Lerp(start, target, smoothstep(interp()));
        return finished;
    }

private:
    Vector2 *place;
    Vector2 start, target;
};

class Draw_Image: public Task {
public:
    Draw_Image(std::vector<Texture> *texs , Texture tex):
        texs(texs),
        tex(tex)
    {}

    virtual bool update(Env env) override
    {
        texs->push_back(tex);
        return true;
    }

private:
    std::vector<Texture> *texs;
    Texture tex;
};

class Draw: public Wait {
private:
    std::vector<Rectangle> *points;
    int cur;
    float size;
public:
    std::vector<Vector2> q;
    Draw(std::vector<Rectangle> *points, std::vector<Vector2> q, float size, float duration):
        Wait(duration),
        points(points),
        q(q),
        size(size)
    {}

    virtual bool update(Env env) override
    {
        if (!started) cur = 0;
        bool finished = Wait::update(env);
        int next = Lerp(cur, q.size(), interp());
        for(int i=cur;i<next;i++){
            points->push_back({q.at(i).x-size/2,q.at(i).y-size/2,size,size});
        }
	cur=next;
        return finished;
    }
};

typedef struct {
    size_t size;
    Font font;
    Task *task;
    bool finished;
    Vector2 position;
    std::vector<Rectangle> *points;
    std::vector<Texture> *texs;
    bool draw_mode;
    Draw* dr;
} Plug;

static Plug *p;

static void load_assets(void)
{
    p->font = LoadFontEx("./assets/fonts/Vollkorn-Regular.ttf", FONT_SIZE, NULL, 0);
}

static void unload_assets(void)
{
    UnloadFont(p->font);
}

extern "C" {

#define PLUG(name, ret, ...) ret name(__VA_ARGS__);
LIST_OF_PLUGS
#undef PLUG

void plug_reset(void)
{
    p->finished = false;
    if (p->task) delete p->task;
    p->points->clear();
    p->texs->clear();
    std::vector<Vector2> v = std::vector<Vector2>();
    v.push_back({10.0,10.0});
    v.push_back({10.0,13.0});
    v.push_back({12.0,14.0});
    Texture tex = LoadTexture("/home/pepa3/Documents/pepe-close.png");
    if(!p->dr) p->dr = new Draw(p->points, v, 3, 1.f);
    p->task = new Seq {
        new Move_Vec2(&p->position, {200.0, 200.0}, 0.5f),
        new Draw_Image(p->texs, tex),
        p->dr,
        new Move_Vec2(&p->position, {200.0, 0.0}, 0.5f),
        new Move_Vec2(&p->position, {0.0, 200.0}, 0.5f),
        new Move_Vec2(&p->position, {0.0, 0.0}, 0.5f)
    };
    p->position = {0, 0};
    p->draw_mode=false;
}

void plug_init(void)
{
    p = (Plug*)malloc(sizeof(*p));
    assert(p != NULL);
    memset(p, 0, sizeof(*p));
    p->size = sizeof(*p);
    p->points = new std::vector<Rectangle>();
    p->texs = new std::vector<Texture>();

    load_assets();
    plug_reset();
}

void *plug_pre_reload(void)
{
    unload_assets();
    return p;
}

void plug_post_reload(void *state)
{
    p = (Plug*)state;
    if (p->size < sizeof(*p)) {
        TraceLog(LOG_INFO, "Migrating plug state schema %zu bytes -> %zu bytes", p->size, sizeof(*p));
        p = (Plug*)realloc(p, sizeof(*p));
        p->size = sizeof(*p);
    }

    load_assets();
}

void plug_update(Env env)
{
    if(IsKeyPressed(KEY_E)){
        p->draw_mode=!p->draw_mode;
    }
    if(!p->draw_mode){
        p->finished = p->task->update(env);
    }else if(IsMouseButtonDown(MOUSE_BUTTON_LEFT)){
        p->dr->q.push_back(GetMousePosition());
    }

    Color background_color = ColorFromHSV(0, 0, 0.05);
    Color foreground_color = ColorFromHSV(0, 0, 0.95);

    ClearBackground(background_color);

    for(auto& t : *p->texs){
        DrawTextureV(t,{0,0},{0xff,0xff,0xff,0xff});
    }
    for(auto& p : *p->points){
        DrawRectangleRec(p,foreground_color);
    }

    const char *text = "Hello from C++";
    DrawTextEx(p->font, text, p->position, FONT_SIZE, 0, foreground_color);
}

bool plug_finished(void)
{
    return p->finished;
}

}
