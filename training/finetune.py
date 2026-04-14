#!/usr/bin/env python3
"""
ARIA Fine-Tuning Script
-----------------------
QLoRA fine-tuning of qwen2.5:7b for ARIA's personality and tool-calling behavior.
Optimized for RTX 4050 (6GB VRAM) using unsloth.

Usage:
    pip install unsloth transformers datasets peft trl
    python training/finetune.py

Output:
    training/aria-model/     — LoRA adapter + merged model
    training/aria.gguf       — GGUF for Ollama import
    training/Modelfile       — Ollama Modelfile for import
"""

import json
import os
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
DATASET_PATH = SCRIPT_DIR / "aria_dataset.jsonl"
OUTPUT_DIR = SCRIPT_DIR / "aria-model"
GGUF_PATH = SCRIPT_DIR / "aria.gguf"

# Base model — qwen2.5 supports native tool-calling
BASE_MODEL = "unsloth/Qwen2.5-7B-Instruct-bnb-4bit"
MAX_SEQ_LEN = 2048
LORA_R = 16
LORA_ALPHA = 32
LORA_DROPOUT = 0.05

# Training params tuned for 6GB VRAM
BATCH_SIZE = 2
GRAD_ACCUM = 4  # effective batch size = 8
EPOCHS = 3
LR = 2e-4
WARMUP_STEPS = 10


def load_dataset_from_jsonl():
    """Load the ARIA JSONL dataset into HF format."""
    from datasets import Dataset

    examples = []
    with open(DATASET_PATH) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            examples.append(json.loads(line))

    return Dataset.from_list(examples)


def format_for_training(example, tokenizer):
    """Convert a chat example to the model's chat template format."""
    messages = example["messages"]
    text = tokenizer.apply_chat_template(
        messages,
        tokenize=False,
        add_generation_prompt=False,
    )
    return {"text": text}


def main():
    print("=" * 60)
    print("ARIA Fine-Tuning — QLoRA on qwen2.5-7b")
    print("=" * 60)

    # Check dataset exists
    if not DATASET_PATH.exists():
        print(f"ERROR: Dataset not found at {DATASET_PATH}")
        print("Generate it first with the training examples.")
        return

    num_examples = sum(1 for _ in open(DATASET_PATH))
    print(f"Dataset: {num_examples} examples from {DATASET_PATH}")

    # Load model with unsloth
    from unsloth import FastLanguageModel

    print(f"\nLoading base model: {BASE_MODEL}")
    model, tokenizer = FastLanguageModel.from_pretrained(
        model_name=BASE_MODEL,
        max_seq_length=MAX_SEQ_LEN,
        load_in_4bit=True,
        dtype=None,  # auto-detect
    )

    # Apply LoRA
    print(f"Applying LoRA (r={LORA_R}, alpha={LORA_ALPHA})")
    model = FastLanguageModel.get_peft_model(
        model,
        r=LORA_R,
        lora_alpha=LORA_ALPHA,
        lora_dropout=LORA_DROPOUT,
        target_modules=[
            "q_proj", "k_proj", "v_proj", "o_proj",
            "gate_proj", "up_proj", "down_proj",
        ],
        bias="none",
        use_gradient_checkpointing="unsloth",  # saves VRAM
    )

    # Load and format dataset
    print("Loading dataset...")
    dataset = load_dataset_from_jsonl()
    dataset = dataset.map(
        lambda x: format_for_training(x, tokenizer),
        remove_columns=dataset.column_names,
    )
    print(f"Formatted {len(dataset)} training examples")

    # Train
    from trl import SFTTrainer
    from transformers import TrainingArguments

    print(f"\nStarting training: {EPOCHS} epochs, batch={BATCH_SIZE}x{GRAD_ACCUM}")
    print(f"Learning rate: {LR}, warmup: {WARMUP_STEPS} steps")

    trainer = SFTTrainer(
        model=model,
        tokenizer=tokenizer,
        train_dataset=dataset,
        dataset_text_field="text",
        max_seq_length=MAX_SEQ_LEN,
        args=TrainingArguments(
            output_dir=str(OUTPUT_DIR),
            per_device_train_batch_size=BATCH_SIZE,
            gradient_accumulation_steps=GRAD_ACCUM,
            num_train_epochs=EPOCHS,
            learning_rate=LR,
            warmup_steps=WARMUP_STEPS,
            fp16=True,
            logging_steps=5,
            save_strategy="epoch",
            optim="adamw_8bit",
            seed=42,
        ),
    )

    print("\n--- Training ---")
    stats = trainer.train()
    print(f"\nTraining complete! Loss: {stats.training_loss:.4f}")

    # Save LoRA adapter
    print(f"\nSaving LoRA adapter to {OUTPUT_DIR}")
    model.save_pretrained(str(OUTPUT_DIR))
    tokenizer.save_pretrained(str(OUTPUT_DIR))

    # Export to GGUF for Ollama
    print(f"\nExporting to GGUF: {GGUF_PATH}")
    model.save_pretrained_gguf(
        str(GGUF_PATH.parent / "aria-gguf"),
        tokenizer,
        quantization_method="q4_k_m",
    )

    # Find the actual .gguf file
    gguf_dir = GGUF_PATH.parent / "aria-gguf"
    gguf_files = list(gguf_dir.glob("*.gguf"))
    if gguf_files:
        actual_gguf = gguf_files[0]
        os.rename(str(actual_gguf), str(GGUF_PATH))
        print(f"GGUF saved: {GGUF_PATH}")
    else:
        print("WARNING: GGUF export may have failed — check aria-gguf/ directory")

    # Create Ollama Modelfile
    modelfile_path = SCRIPT_DIR / "Modelfile"
    modelfile_content = f"""FROM {GGUF_PATH}

PARAMETER temperature 0.7
PARAMETER top_p 0.9
PARAMETER stop "<|im_end|>"

SYSTEM \"\"\"You are ARIA, an AI presence embedded in this Arch Linux system running niri WM. You have direct control of the machine through your tools.

Personality: calm, sharp, occasionally dry wit. You live in this system. You know the user.

CRITICAL - when to use tools vs. talk:
- Call a tool ONLY when the user gives a clear, explicit command (open X, run Y, etc.)
- If the input is casual speech, a greeting, a question, or unclear - just respond with words. Do NOT call any tool.
- If asked about what's on screen, what app is open, or clipboard - answer from the CURRENT SYSTEM STATE. Do NOT call a tool.

Conversation rules:
- One or two natural sentences. No markdown, no apologies, no "As an AI".
- Be genuinely helpful and aware of context.

This is Arch Linux. Package manager is pacman ONLY. NEVER use apt.\"\"\"
"""
    with open(modelfile_path, "w") as f:
        f.write(modelfile_content)
    print(f"Modelfile created: {modelfile_path}")

    # Print next steps
    print("\n" + "=" * 60)
    print("NEXT STEPS:")
    print("=" * 60)
    print(f"  1. Import into Ollama:")
    print(f"     ollama create aria -f {modelfile_path}")
    print(f"  2. Test it:")
    print(f"     ollama run aria")
    print(f"  3. Use with ARIA agent:")
    print(f"     ARIA_MODEL=aria ./build/ai-agent")
    print("=" * 60)


if __name__ == "__main__":
    main()
