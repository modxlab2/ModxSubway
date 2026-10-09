class Rect {
public:
    float x;
    float y;
    float w;
    float h;

    // Construtor padrão
    Rect()
        : x(0), y(0), w(0), h(0) {
    }

    // Construtor com parâmetros
    Rect(float x1, float y1, float w1, float h1)
        : x(x1), y(y1), w(w1), h(h1) {
    }

    // Construtor de cópia
    Rect(const Rect& v)
        : x(v.x), y(v.y), w(v.w), h(v.h) {
    }

    // Destrutor
    ~Rect() {
    }
};


/*class Rect {
public:
    float x;
    float y;
    float w;
    float h;

    Rect() {
        this->x = 0;
        this->y = 0;
        this->w = 0;
        this->h = 0;
    }

    Rect(float x, float y, float w, float h) {
        this->x = x;
        this->y = y;
        this->w = w;
        this->h = h;
    }

    bool operator==(const Rect &src) const {
        return (src.x == this->x && src.y == this->y && src.h == this->h &&
                src.w == this->w);
    }

    bool operator!=(const Rect &src) const {
        return (src.x != this->x && src.y != this->y && src.h != this->h &&
                src.w != this->w);
    }
};*/
