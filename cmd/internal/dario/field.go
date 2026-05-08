package dario

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// FieldClient talks to a running `dario --web N` REPL via HTTP.
// The C side answers POST /api/chat with rich equation metrics — chamber
// activations, dominant term, dissonance, entropy etc.
type FieldClient struct {
	BaseURL string
	HTTP    *http.Client
}

// NewFieldClient builds a client with sane defaults pointing at
// http://localhost:3001 (matches dario.c's WEB_PORT default).
func NewFieldClient(baseURL string) *FieldClient {
	if baseURL == "" {
		baseURL = "http://localhost:3001"
	}
	baseURL = strings.TrimRight(baseURL, "/")
	return &FieldClient{
		BaseURL: baseURL,
		HTTP:    &http.Client{Timeout: 10 * time.Second},
	}
}

// FieldMetrics is a partial mirror of the JSON returned by /api/chat.
// We keep only the fields used by the dialogue / forum modes; the
// underlying JSON has many more.
type FieldMetrics struct {
	Code         string             `json:"code"`
	Words        string             `json:"words"`
	Dominant     int                `json:"dominant"`
	DominantName string             `json:"dominant_name"`
	Dissonance   float64            `json:"dissonance"`
	Tau          float64            `json:"tau"`
	Resonance    float64            `json:"resonance"`
	Entropy      float64            `json:"entropy"`
	Emergence    float64            `json:"emergence"`
	Chambers     map[string]float64 `json:"chambers"`
	Step         int                `json:"step"`
	// Raw is the full JSON body, in case a caller wants more fields.
	Raw map[string]interface{} `json:"-"`
}

// Chat POSTs `text` to /api/chat and returns the parsed metrics.
//
// Used by dialogue/forum to (a) feed model output back into the dario
// equation field for KK absorption, and (b) retrieve "field-words" that
// can prime the next voice's prompt.
func (c *FieldClient) Chat(ctx context.Context, text string) (*FieldMetrics, error) {
	body, _ := json.Marshal(map[string]string{"text": text})
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		c.BaseURL+"/api/chat", bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return nil, fmt.Errorf("dario field POST failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 != 2 {
		buf, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("dario field returned %s: %s", resp.Status, buf)
	}
	buf, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	var m FieldMetrics
	if err := json.Unmarshal(buf, &m); err != nil {
		return nil, fmt.Errorf("decoding field metrics: %w (body=%s)", err, buf)
	}
	// Stash the full payload too.
	_ = json.Unmarshal(buf, &m.Raw)
	return &m, nil
}

// Available returns true if the dario web REPL is reachable. Used by
// dialogue/forum to silently degrade to KK-only mode when no field is up.
func (c *FieldClient) Available(ctx context.Context) bool {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.BaseURL+"/api/kernel", nil)
	if err != nil {
		return false
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return false
	}
	resp.Body.Close()
	return resp.StatusCode/100 == 2
}
